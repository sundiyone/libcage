/*
 * Copyright (c) 2009, Yuuki Takano (ytakanoster@gmail.com).
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the writers nor the names of its contributors may be
 *    used to endorse or promote products derived from this software without
 *    specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "dht.hpp"

#include "ping.hpp"

#include <boost/foreach.hpp>

namespace libcage {
        const int       dht::num_find_node    = 6;
        const int       dht::max_query        = 3;
        const int       dht::query_timeout    = 3;
        const int       dht::restore_interval = 360;
        const int       dht::timer_interval   = 180;


        size_t
        hash_value(const dht::_id &i)
        {
                return i.id->hash_value();
        }

        size_t
        hash_value(const dht::id_key &ik)
        {
                size_t h;

                h = ik.id->hash_value();

                for (int i = 0; i < ik.keylen; i++)
                        boost::hash_combine(h, ik.key[i]);

                return h;
        }

        dht::dht(const uint160_t &id, timer &t, peers &p,
                 const natdetector &nat, udphandler &udp, dtun &dt) :
                rttable(id, t, p),
                m_id(id),
                m_timer(t),
                m_peers(p),
                m_nat(nat),
                m_udp(udp),
                m_dtun(dt),
                m_is_dtun(true),
                m_last_restore(0),
                m_timer_dht(*this),
                m_join(*this)
        {

        }

        dht::~dht()
        {

        }

        void
        dht::ping_func::operator() (bool result, cageaddr &addr)
        {
                if (result) {
                        send_ping_tmpl<msg_dht_ping>(addr, nonce, type_dht_ping,
                                                     p_dht->m_id,
                                                     p_dht->m_udp);
                }
        }

        void
        dht::send_ping(cageaddr &dst, uint32_t nonce)
        {
                try {
                        if (m_is_dtun)
                                m_peers.get_addr(dst.id);

                        send_ping_tmpl<msg_dht_ping>(dst, nonce, type_dht_ping,
                                                     m_id, m_udp);
                } catch (std::out_of_range) {
                        ping_func func;

                        func.dst   = dst;
                        func.nonce = nonce;
                        func.p_dht = this;

                        m_dtun.request(*dst.id, func);
                }
        }

        void
        dht::recv_ping(void *msg, sockaddr *from, int fromlen)
        {
                recv_ping_tmpl<msg_dht_ping,
                        msg_dht_ping_reply>(msg, from, fromlen,
                                            type_dht_ping_reply,
                                            m_id, m_udp, m_peers);
        }

        void
        dht::recv_ping_reply(void *msg, sockaddr *from, int fromlen)
        {
                recv_ping_reply_tmpl<msg_dht_ping_reply>(msg, from, fromlen,
                                                         m_id, m_peers, this);
        }

        void
        dht::send_msg(msg_hdr *msg, uint16_t len, uint8_t type, cageaddr &dst)
        {
                msg->magic = htons(MAGIC_NUMBER);
                msg->ver   = CAGE_VERSION;
                msg->type  = type;
                msg->len   = htons(len);

                dst.id->to_binary(msg->dst, sizeof(msg->dst));
                m_id.to_binary(msg->src, sizeof(msg->src));

                if (dst.domain == domain_inet) {
                        in_ptr in;
                        in = boost::get<in_ptr>(dst.saddr);

                        m_udp.sendto(msg, len, (sockaddr*)in.get(),
                                     sizeof(sockaddr_in));
                } else if (dst.domain == domain_inet6) {
                        in6_ptr in6;
                        in6 = boost::get<in6_ptr>(dst.saddr);

                        m_udp.sendto(msg, len, (sockaddr*)in6.get(),
                                     sizeof(sockaddr_in6));
                }
        }

        void
        dht::find_nv(const uint160_t &dst, callback_func func,
                     bool is_find_value, void *key = NULL, int keylen = 0)
        {
                query_ptr q(new query);

                lookup(dst, num_find_node, q->nodes);

                if (q->nodes.size() == 0) {
                        if (is_find_value) {
                                callback_find_value f;
                                f = boost::get<callback_find_value>(func);
                                f(false, NULL, 0);
                        } else {
                                callback_find_node f;
                                f = boost::get<callback_find_node>(func);
                                f(q->nodes);
                        }
                        return;
                }

                id_ptr p_dst(new uint160_t);
                *p_dst = dst;

                q->dst           = p_dst;
                q->num_query     = 0;
                q->is_find_value = is_find_value;
                q->func          = func;

                if (is_find_value) {
                        q->key = boost::shared_array<char>(new char[keylen]);
                        q->keylen = keylen;

                        memcpy(q->key.get(), key, keylen);
                }

                if (is_find_value) {
                        q->key    = boost::shared_array<char>(new char[keylen]);
                        q->keylen = keylen;
                        memcpy(q->key.get(), key, keylen);
                }

                // add my id
                _id i;
                i.id = id_ptr(new uint160_t);
                *i.id = m_id;

                q->sent.insert(i);


                uint32_t nonce;
                do {
                        nonce = mrand48();
                } while (m_query.find(nonce) != m_query.end());

                q->nonce = nonce;
                m_query[nonce] = q;


                send_find(q);
        }

        void
        dht::find_node(const uint160_t &dst, callback_find_node func)
        {
                node_state state = m_nat.get_state();
                if (state == node_symmetric || state == node_undefined ||
                    state == node_nat)
                        return;

                find_nv(dst, func, false);
        }

        void
        dht::send_find(query_ptr q)
        {
                BOOST_FOREACH(cageaddr &addr, q->nodes) {
                        if (q->num_query >= max_query) {
                                break;
                        }

                        _id i;
                        i.id = addr.id;

                        if (q->sent.find(i) != q->sent.end()) {
                                continue;
                        }

                        // start timer
                        timeval   tval;
                        timer_ptr t(new timer_query);

                        t->nonce = q->nonce;
                        t->id    = i;
                        t->p_dht = this;

                        tval.tv_sec  = query_timeout;
                        tval.tv_usec = 0;

                        q->timers[i] = t;
                        q->sent.insert(i);

                        m_timer.set_timer(t.get(), &tval);


                        // send find node
                        if (q->is_find_value) {
                                send_find_value(addr, q);
                        } else {
                                send_find_node(addr, q);
                        }

                        q->num_query++;
                }

                if (q->num_query == 0) {
                        // call callback functions
                        if (q->is_find_value) {
                                cageaddr addr1, addr2;
                                callback_find_value func;
                                func = boost::get<callback_find_value>(q->func);
                                func(false, NULL, 0);
                        } else {
                                callback_find_node func;
                                func = boost::get<callback_find_node>(q->func);
                                func(q->nodes);
                        }

                        // stop all timers
                        boost::unordered_map<_id, timer_ptr>::iterator it;
                        for (it = q->timers.begin(); it != q->timers.end();
                             ++it) {
                                m_timer.unset_timer(it->second.get());
                        }

                        // remove query
                        m_query.erase(q->nonce);
                }
        }

        void
        dht::find_node_func::operator() (bool result, cageaddr &addr)
        {
                if (! result)
                        return;

                msg_dht_find_node msg;

                memset(&msg, 0, sizeof(msg));

                msg.nonce  = htonl(nonce);
                msg.domain = htons(addr.domain);

                dst->to_binary(msg.id, sizeof(msg.id));

                p_dht->send_msg(&msg.hdr, sizeof(msg), type_dht_find_node,
                                addr);
        }

        void
        dht::send_find_node(cageaddr &dst, query_ptr q)
        {
                try {
                        if (m_is_dtun && ! dst.id->is_zero())
                                m_peers.get_addr(dst.id);

                        msg_dht_find_node msg;

                        memset(&msg, 0, sizeof(msg));

                        msg.nonce  = htonl(q->nonce);
                        msg.domain = htons(dst.domain);

                        q->dst->to_binary(msg.id, sizeof(msg.id));

                        send_msg(&msg.hdr, sizeof(msg), type_dht_find_node,
                                 dst);
                } catch (std::out_of_range) {
                        find_node_func func;

                        func.dst   = q->dst;
                        func.nonce = q->nonce;
                        func.p_dht = this;

                        m_dtun.request(*dst.id, func);
                }
        }

        void
        dht::timer_query::operator() ()
        {
                query_ptr q = p_dht->m_query[nonce];
                timer_ptr t = q->timers[id];
                uint160_t zero;

                q->sent.insert(id);
                q->num_query--;
                q->timers.erase(id);

                zero.fill_zero();

                if (*id.id != zero) {
                        std::vector<cageaddr> tmp;
                        
                        tmp = q->nodes;
                        q->nodes.clear();
                        BOOST_FOREACH(cageaddr &addr, tmp) {
                                if (*addr.id != *id.id)
                                        q->nodes.push_back(addr);
                        }

                        p_dht->m_peers.add_timeout(id.id);
                        p_dht->remove(*id.id);
                }

                // send find node
                p_dht->send_find(q);
        }

        void
        dht::recv_find_node(void *msg, sockaddr *from)
        {
                msg_dht_find_node_reply *reply;
                msg_dht_find_node       *req;
                std::vector<cageaddr>    nodes;
                uint160_t dst;
                uint160_t zero;
                uint160_t id;
                uint16_t  domain;
                cageaddr  addr;
                char      buf[1024 * 2];
                int       len = 0;

                req = (msg_dht_find_node*)msg;

                zero.fill_zero();
                dst.from_binary(req->hdr.dst, sizeof(req->hdr.dst));
                if (dst != m_id && dst != zero)
                        return;

                domain = ntohs(req->domain);
                if (domain != m_udp.get_domain())
                        return;

                addr = new_cageaddr(&req->hdr, from);

                // add to rttable and request cache
                m_peers.add_node(addr);
                add(addr);

                // lookup
                id.from_binary(req->id, sizeof(req->id));
                lookup(id, num_find_node, nodes);

                // send reply
                memset(buf, 0, sizeof(buf));
                reply = (msg_dht_find_node_reply*)buf;

                reply->nonce  = req->nonce;
                reply->domain = req->domain;
                reply->num    = nodes.size();

                memcpy(reply->id, req->id, sizeof(reply->id));

                if (domain == domain_inet) {
                        msg_inet *min;

                        len = sizeof(msg_inet) * nodes.size() +
                                sizeof(*reply) - sizeof(reply->addrs);

                        min   = (msg_inet*)reply->addrs;
                        write_nodes_inet(min, nodes);
                } else if (domain == domain_inet6) {
                        msg_inet6 *min6;

                        len = sizeof(msg_inet6) * nodes.size() +
                                sizeof(*reply) - sizeof(reply->addrs);

                        min6  = (msg_inet6*)reply->addrs;
                        write_nodes_inet6(min6, nodes);
                } else {
                        return;
                }

                send_msg(&reply->hdr, len, type_dht_find_node_reply, addr);
        }

        void
        dht::recv_find_node_reply(void *msg, int len, sockaddr *from)
        {
                msg_dht_find_node_reply *reply;
                std::vector<cageaddr>    nodes;
                query_ptr q;
                uint160_t dst, id;
                cageaddr  addr;
                uint32_t  nonce;
                uint16_t  domain;
                int       size;
                _id       c_id;


                reply = (msg_dht_find_node_reply*)msg;

                nonce = ntohl(reply->nonce);
                if (m_query.find(nonce) == m_query.end())
                        return;

                dst.from_binary(reply->hdr.dst, sizeof(reply->hdr.dst));
                if (dst != m_id)
                        return;

                id.from_binary(reply->id, sizeof(reply->id));
                q = m_query[nonce];

                if (*q->dst != id)
                        return;

                if (q->is_find_value)
                        return;

                addr = new_cageaddr(&reply->hdr, from);

                // add rttable and request cache
                m_peers.add_node(addr);
                add(addr);

                // stop timer
                c_id.id = addr.id;
                if (q->timers.find(c_id) == q->timers.end()) {
                        timer_ptr t;
                        id_ptr    zero(new uint160_t);

                        zero->fill_zero();
                        c_id.id = zero;
                        if (q->timers.find(c_id) == q->timers.end())
                                return;
                        
                        t = q->timers[c_id];
                        m_timer.unset_timer(t.get());
                        q->timers.erase(c_id);
                } else {
                        timer_ptr t;
                        t = q->timers[c_id];
                        m_timer.unset_timer(t.get());
                        q->timers.erase(c_id);
                }

                // read nodes
                domain = ntohs(reply->domain);
                if (domain == domain_inet) {
                        msg_inet *min;
                        size = sizeof(*reply) - sizeof(reply->addrs) +
                                sizeof(*min) * reply->num;

                        if (size != len)
                                return;

                        min = (msg_inet*)reply->addrs;

                        read_nodes_inet(min, reply->num, nodes, from, m_peers);
                } else if (domain == domain_inet6) {
                        msg_inet6 *min6;
                        size = sizeof(*reply) - sizeof(reply->addrs) +
                                sizeof(*min6) * reply->num;

                        if (size != len)
                                return;

                        min6 = (msg_inet6*)reply->addrs;

                        read_nodes_inet6(min6, reply->num, nodes, from,
                                         m_peers);
                } else {
                        return;
                }

                _id i;

                i.id = addr.id;

                q->sent.insert(i);
                q->num_query--;

                // sort
                compare cmp;
                cmp.m_id = &id;
                std::sort(nodes.begin(), nodes.end(), cmp);

                // merge
                std::vector<cageaddr> tmp;

                tmp = q->nodes;
                q->nodes.clear();

                merge_nodes(id, q->nodes, tmp, nodes, num_find_node);

                // send
                send_find(q);
        }

        void
        dht::find_node(std::string host, int port, callback_find_node func)
        {
                node_state state = m_nat.get_state();
                if (state == node_symmetric || state == node_undefined ||
                    state == node_nat)
                        return;


                sockaddr_storage saddr;

                if (! m_udp.get_sockaddr(&saddr, host, port))
                        return;

                find_node((sockaddr*)&saddr, func);
        }

        void
        dht::find_node(sockaddr *saddr, callback_find_node func)
        {
                // initialize query
                query_ptr q(new query);
                id_ptr    dst(new uint160_t);

                *dst = m_id;

                q->dst           = dst;
                q->num_query     = 1;
                q->is_find_value = false;
                q->func          = func;

                // add my id
                _id i;
                i.id = id_ptr(new uint160_t);
                *i.id = m_id;

                q->sent.insert(i);

                uint32_t nonce;
                do {
                        nonce = mrand48();
                } while (m_query.find(nonce) != m_query.end());

                q->nonce = nonce;
                m_query[nonce] = q;


                // start timer
                timeval   tval;
                timer_ptr t(new timer_query);
                id_ptr    zero(new uint160_t);
                _id       zero_id;

                zero->fill_zero();
                zero_id.id = zero;

                t->nonce = q->nonce;
                t->id    = zero_id;
                t->p_dht = this;

                tval.tv_sec  = query_timeout;
                tval.tv_usec = 0;

                q->timers[zero_id] = t;

                m_timer.set_timer(t.get(), &tval);


                // send find node
                cageaddr addr;
                addr.id = zero;

                addr.domain = m_udp.get_domain();
                if (addr.domain == domain_inet) {
                        in_ptr in(new sockaddr_in);
                        memcpy(in.get(), saddr, sizeof(sockaddr_in));
                        addr.saddr = in;
                } else if (addr.domain == domain_inet6) {
                        in6_ptr in6(new sockaddr_in6);
                        memcpy(in6.get(), saddr, sizeof(sockaddr_in6));
                        addr.saddr = in6;
                }

                send_find_node(addr, q);
        }

        void
        dht::set_enabled_dtun(bool flag)
        {
                m_is_dtun = flag;
        }

        void
        dht::store(const uint160_t &id, void *key, uint16_t keylen,
                   void *value, uint16_t valuelen, uint16_t ttl)
        {
                store_func func;
                id_ptr     p_id(new uint160_t);

                *p_id = id;

                func.key      = boost::shared_array<char>(new char[keylen]);
                func.value    = boost::shared_array<char>(new char[valuelen]);
                func.id       = p_id;
                func.keylen   = keylen;
                func.valuelen = valuelen;
                func.ttl      = ttl;
                func.p_dht    = this;

                memcpy(func.key.get(), key, keylen);
                memcpy(func.value.get(), value, valuelen);

                find_node(id, func);
        }

        void
        dht::store_func::operator() (std::vector<cageaddr>& nodes)
        {
                msg_dht_store *msg;
                int            size;
                char           buf[1024 * 4];
                char          *p_key, *p_value;

                size = sizeof(*msg) - sizeof(msg->data) + keylen + valuelen;

                if (size > (int)sizeof(buf))
                        return;

                msg = (msg_dht_store*)buf;

                msg->keylen   = htons(keylen);
                msg->valuelen = htons(valuelen);
                msg->ttl      = htons(ttl);

                id->to_binary(msg->id, sizeof(msg->id));

                p_key   = (char*)msg->data;
                p_value = p_key + keylen;

                memcpy(p_key, key.get(), keylen);
                memcpy(p_value, value.get(), valuelen);

                // send store
                BOOST_FOREACH(cageaddr &addr, nodes) {
                        if (*addr.id == p_dht->m_id)
                                continue;

                        p_dht->send_msg(&msg->hdr, size, type_dht_store, addr);
                }
        }

        void
        dht::recv_store(void *msg, int len, sockaddr *from)
        {
                msg_dht_store *req;
                cageaddr  addr;
                uint160_t dst;
                uint16_t  keylen;
                uint16_t  valuelen;
                uint16_t  ttl;
                int       size;

                req = (msg_dht_store*)msg;

                dst.from_binary(req->hdr.dst, sizeof(req->hdr.dst));
                if (dst != m_id)
                        return;

                keylen   = ntohs(req->keylen);
                valuelen = ntohs(req->valuelen);
                ttl      = ntohs(req->ttl);

                size = sizeof(*req) - sizeof(req->data) + keylen + valuelen;

                if (size != len)
                        return;

                addr = new_cageaddr(&req->hdr, from);

                // add to rttable and request cache
                add(addr);
                m_peers.add_node(addr);

                
                // store data
                boost::shared_array<char> key(new char[keylen]);
                boost::shared_array<char> value(new char[valuelen]);
                id_ptr id(new uint160_t);
                id_key ik;

                id->from_binary(req->id, sizeof(req->id));
                memcpy(key.get(), req->data, keylen);
                memcpy(value.get(), (char*)req->data + keylen, valuelen);

                ik.key    = key;
                ik.keylen = keylen;
                ik.id     = id;

                boost::unordered_map<id_key, stored_data>::iterator it;
                it = m_stored.find(ik);

                if (it != m_stored.end()) {
                        if (it->second.valuelen == valuelen &&
                            memcmp(it->second.value.get(),
                                   value.get(), valuelen) == 0) {
                                _id i;

                                i.id = addr.id;

                                it->second.ttl         = ttl;
                                it->second.stored_time = time(NULL);
                                it->second.recvd.insert(i);
                        }
                } else {
                        stored_data data;
                        _id i;

                        i.id = addr.id;

                        data.key         = key;
                        data.value       = value;
                        data.keylen      = keylen;
                        data.valuelen    = valuelen;
                        data.ttl         = ttl;
                        data.stored_time = time(NULL);
                        data.id          = id;

                        data.recvd.insert(i);

                        m_stored[ik] = data;
                }
        }

        void
        dht::find_value(const uint160_t &dst, void *key, uint16_t keylen,
                        callback_find_value func)
        {
                node_state state = m_nat.get_state();
                if (state == node_symmetric || state == node_undefined ||
                    state == node_nat)
                        return;


                find_nv(dst, func, true, key, keylen);
        }

        void
        dht::find_value_func::operator() (bool result, cageaddr &addr)
        {
                if (! result)
                        return;

                msg_dht_find_value *msg;
                int  size;
                char buf[1024 * 4];

                size = sizeof(*msg) - sizeof(msg->key)
                        + keylen;

                if (size > (int)sizeof(buf))
                        return;

                msg = (msg_dht_find_value*)buf;

                memset(msg, 0, size);

                msg->nonce  = htonl(nonce);
                msg->domain = htons(addr.domain);
                msg->keylen = htons(keylen);

                memcpy(msg->key, key.get(), keylen);

                dst->to_binary(msg->id, sizeof(msg->id));

                p_dht->send_msg(&msg->hdr, size, type_dht_find_value, addr);
        }

        void
        dht::send_find_value(cageaddr &dst, query_ptr q)
        {
                try {
                        if (m_is_dtun && ! dst.id->is_zero())
                                m_peers.get_addr(dst.id);

                        msg_dht_find_value *msg;
                        int  size;
                        char buf[1024 * 4];

                        size = sizeof(*msg) - sizeof(msg->key)
                                + q->keylen;

                        if (size > (int)sizeof(buf))
                                return;

                        msg = (msg_dht_find_value*)buf;

                        memset(msg, 0, size);

                        msg->nonce  = htonl(q->nonce);
                        msg->domain = htons(dst.domain);
                        msg->keylen = htons(q->keylen);

                        memcpy(msg->key, q->key.get(), q->keylen);

                        q->dst->to_binary(msg->id, sizeof(msg->id));

                        send_msg(&msg->hdr, size, type_dht_find_value,
                                 dst);
                } catch (std::out_of_range) {
                        find_value_func func;

                        func.key    = q->key;
                        func.keylen = q->keylen;
                        func.dst    = q->dst;
                        func.nonce  = q->nonce;
                        func.p_dht  = this;

                        m_dtun.request(*dst.id, func);
                }
        }

        void
        dht::recv_find_value(void *msg, int len, sockaddr *from)
        {
                boost::shared_array<char> key;
                msg_dht_find_value_reply *reply;
                msg_dht_find_value       *req;
                cageaddr  addr;
                uint160_t dst;
                uint16_t  size;
                uint16_t  keylen;
                id_key    ik;
                id_ptr    id(new uint160_t);
                char      buf[1024 * 4];


                req = (msg_dht_find_value*)msg;

                dst.from_binary(req->hdr.dst, sizeof(req->hdr.dst));
                if (dst != m_id)
                        return;

                keylen = ntohs(req->keylen);

                size = sizeof(*req) - sizeof(req->key) + keylen;
                if (size != len)
                        return;

                // add to rttable and request cache
                addr = new_cageaddr(&req->hdr, from);
                add(addr);
                m_peers.add_node(addr);

                // lookup stored data
                key = boost::shared_array<char>(new char[keylen]);
                memcpy(key.get(), req->key, keylen);

                id->from_binary(req->id, sizeof(req->id));

                ik.keylen = keylen;
                ik.key    = key;
                ik.id     = id;

                boost::unordered_map<id_key, stored_data>::iterator it;
                it = m_stored.find(ik);

                reply = (msg_dht_find_value_reply*)buf;

                if (it != m_stored.end()) {
                        // reply data
                        msg_data *data;

                        size = sizeof(*reply) - sizeof(reply->data) +
                                sizeof(*data) - sizeof(data->data) +
                                it->second.keylen + it->second.valuelen;

                        memset(reply, 0, size);

                        reply->nonce = req->nonce;
                        reply->flag  = 1;

                        memcpy(reply->id, req->id, sizeof(reply->id));

                        data  = (msg_data*)reply->data;

                        data->keylen   = htons(it->second.keylen);
                        data->valuelen = htons(it->second.valuelen);

                        memcpy(data->data, it->second.key.get(),
                               it->second.keylen);
                        memcpy((char*)data->data + it->second.keylen,
                               it->second.value.get(), it->second.valuelen);

                        send_msg(&reply->hdr, size, type_dht_find_value_reply,
                                 addr);
                } else {
                        // reply nodes
                        msg_nodes *data;
                        std::vector<cageaddr>    nodes;
                        uint16_t domain;

                        lookup(*id, num_find_node, nodes);

                        domain = ntohs(req->domain);

                        if (domain == domain_inet) {
                                msg_inet *min;

                                size = sizeof(*reply) - sizeof(reply->data) +
                                        sizeof(*data) - sizeof(data->addrs) +
                                        sizeof(*min) * nodes.size();

                                memset(reply, 0, size);

                                reply->nonce = req->nonce;
                                reply->flag  = 0;

                                memcpy(reply->id, req->id, sizeof(reply->id));
                                
                                data = (msg_nodes*)reply->data;

                                data->num    = nodes.size();
                                data->domain = req->domain;

                                min = (msg_inet*)data->addrs;
                                write_nodes_inet(min, nodes);
                        } else if (domain == domain_inet6) {
                                msg_inet6 *min6;

                                size = sizeof(*reply) - sizeof(reply->data) +
                                        sizeof(*data) - sizeof(data->addrs) +
                                        sizeof(*min6) * nodes.size();

                                memset(reply, 0, size);

                                reply->nonce = req->nonce;
                                reply->flag  = 0;

                                memcpy(reply->id, req->id, sizeof(reply->id));
                                
                                data = (msg_nodes*)reply->data;

                                data->num    = nodes.size();
                                data->domain = req->domain;

                                min6 = (msg_inet6*)data->addrs;
                                write_nodes_inet6(min6, nodes);
                        } else {
                                return;
                        }

                        send_msg(&reply->hdr, size, type_dht_find_value_reply,
                                 addr);
                }
        }

        void
        dht::recv_find_value_reply(void *msg, int len, sockaddr *from)
        {
                boost::unordered_map<uint32_t, query_ptr>::iterator it;
                msg_dht_find_value_reply *reply;
                cageaddr  addr;
                timer_ptr t;
                query_ptr q;
                uint160_t dst;
                uint160_t id;
                uint32_t  nonce;
                int       size;
                _id       i;

                reply = (msg_dht_find_value_reply*)msg;

                dst.from_binary(reply->hdr.dst, sizeof(reply->hdr.dst));
                if (dst != m_id)
                        return;

                nonce = ntohl(reply->nonce);

                it = m_query.find(nonce);
                if (it == m_query.end())
                        return;

                q = it->second;

                id.from_binary(reply->id, sizeof(reply->id));
                if (id != *q->dst)
                        return;

                addr = new_cageaddr(&reply->hdr, from);

                i.id = addr.id;
                if (q->timers.find(i) == q->timers.end())
                        return;

                if (! q->is_find_value)
                        return;

                // stop timer
                t = q->timers[i];
                m_timer.unset_timer(t.get());
                q->timers.erase(i);

                // add to rttale and request cache
                add(addr);
                m_peers.add_node(addr);


                q->sent.insert(i);
                q->num_query--;

                if (reply->flag == 1) {
                        msg_data *data;
                        uint16_t  keylen;
                        uint16_t  valuelen;
                        char     *key, *value;

                        size = sizeof(*reply) - sizeof(reply->data) +
                                sizeof(*data) - sizeof(data->data);

                        if (len < size)
                                return;

                        data = (msg_data*)reply->data;

                        keylen   = ntohs(data->keylen);
                        valuelen = ntohs(data->valuelen);

                        size += keylen + valuelen;

                        if (size != len)
                                return;

                        key   = (char*)data->data;
                        value = key + keylen;

                        if (keylen != q->keylen)
                                return;

                        if (memcmp(key, q->key.get(), keylen) != 0)
                                return;

                        // call callback function
                        callback_find_value func;

                        func = boost::get<callback_find_value>(q->func);
                        func(true, value, valuelen);

                        // stop all timer
                        boost::unordered_map<_id, timer_ptr>::iterator it_tm;
                        for (it_tm = q->timers.begin();
                             it_tm != q->timers.end(); ++it_tm) {
                                m_timer.unset_timer(it_tm->second.get());
                        }

                        // remove query
                        m_query.erase(nonce);
                } else if (reply->flag == 0) {
                        std::vector<cageaddr> nodes;
                        msg_nodes *addrs;
                        uint16_t   domain;

                        size = sizeof(*reply) - sizeof(reply->data) +
                                sizeof(*addrs) - sizeof(addrs->addrs);

                        if (len < size)
                                return;

                        addrs = (msg_nodes*)reply->data;

                        // read nodes
                        domain = ntohs(addrs->domain);
                        if (domain == domain_inet) {
                                msg_inet *min;
                                
                                size += addrs->num * sizeof(*min);

                                if (size != len)
                                        return;

                                min = (msg_inet*)addrs->addrs;

                                read_nodes_inet(min, addrs->num, nodes, from,
                                                m_peers);
                        } else if (domain == domain_inet6) {
                                msg_inet6 *min6;

                                size += addrs->num * sizeof(*min6);

                                if (size != len)
                                        return;

                                min6 = (msg_inet6*)addrs->addrs;

                                read_nodes_inet6(min6, addrs->num, nodes, from,
                                                 m_peers);
                        } else {
                                return;
                        }

                        // sort
                        compare cmp;
                        cmp.m_id = &id;
                        std::sort(nodes.begin(), nodes.end(), cmp);

                        // merge
                        std::vector<cageaddr> tmp;

                        tmp = q->nodes;
                        q->nodes.clear();

                        merge_nodes(id, q->nodes, tmp, nodes, num_find_node);

                        // send
                        send_find(q);
                }
        }

        void
        dht::refresh()
        {
                boost::unordered_map<id_key, stored_data>::iterator it;
                time_t now = time(NULL);

                for(it = m_stored.begin(); it != m_stored.end();) {
                        time_t diff;
                        diff = now - it->second.stored_time;
                        if (diff > it->second.ttl)
                                m_stored.erase(it++);
                        else
                                ++it;
                }
        }

        void
        dht::restore_func::operator() (std::vector<cageaddr> &n)
        {
                boost::unordered_map<id_key, stored_data>::iterator it;
                std::vector<cageaddr> nodes;
                time_t now = time(NULL);

                for(it = p_dht->m_stored.begin();
                    it != p_dht->m_stored.end();) {
                        msg_dht_store *msg;
                        uint16_t       ttl;
                        int            size;
                        char           buf[1024 * 4];
                        char          *p_key, *p_value;
                        bool           me = false;
                        time_t         diff;
                        
                        p_dht->lookup(*it->second.id, num_find_node, nodes);

                        if (nodes.size() == 0)
                                continue;

                        size = sizeof(*msg) - sizeof(msg->data) +
                                it->second.keylen + it->second.valuelen;

                        if (size > (int)sizeof(buf))
                                continue;

                        msg = (msg_dht_store*)buf;

                        diff = now - it->second.stored_time;
                        if (diff >= it->second.ttl)
                                continue;

                        ttl = it->second.ttl - diff;

                        msg->keylen   = htons(it->second.keylen);
                        msg->valuelen = htons(it->second.valuelen);
                        msg->ttl      = htons(ttl);

                        it->second.id->to_binary(msg->id, sizeof(msg->id));

                        p_key   = (char*)msg->data;
                        p_value = p_key + it->second.keylen;

                        memcpy(p_key, it->second.key.get(), 
                               it->second.keylen);
                        memcpy(p_value, it->second.value.get(),
                               it->second.valuelen);


                        BOOST_FOREACH(cageaddr &addr, nodes) {
                                if (p_dht->m_id == *addr.id) {
                                        me = true;
                                        continue;
                                }

                                _id    i;

                                i.id = addr.id;
                                if (it->second.recvd.find(i) !=
                                    it->second.recvd.end()) {
                                        continue;
                                }

                                p_dht->send_msg(&msg->hdr, size, type_dht_store,
                                                addr);
                        }

                        if (! me)
                                p_dht->m_stored.erase(it++);
                        else
                                ++it;
                }
        }

        void
        dht::restore()
        {
                node_state state = m_nat.get_state();
                if (state == node_symmetric || state == node_undefined ||
                    state == node_nat)
                        return;


                time_t diff;
                diff = time(NULL) - m_last_restore;

                if (diff >= restore_interval) {
                        restore_func func;

                        func.p_dht = this;
                        find_node(m_id, func);
                }
        }

        static void
        no_action(std::vector<cageaddr> &nodes)
        {

        }

        void
        dht::dht_join::operator() ()
        {
                if (m_dht.is_zero()) {
                        node_state state = m_dht.m_nat.get_state();
                        if (state == node_global ||
                            state == node_cone) {
                                try {
                                        cageaddr addr;

                                        addr = m_dht.m_peers.get_first();

                                        if (addr.domain == domain_inet) {
                                                in_ptr in;
                                                in = boost::get<in_ptr>(addr.saddr);
                                                m_dht.find_node((sockaddr*)in.get(), &no_action);
                                        } else if (addr.domain ==
                                                   domain_inet6) {
                                                in6_ptr in6;
                                                in6 = boost::get<in6_ptr>(addr.saddr);
                                                m_dht.find_node((sockaddr*)in6.get(), &no_action);
                                        }
                                } catch (std::out_of_range) {
                                }
                        }

                        m_interval = 3;
                } else {
                        m_interval = 60;
                }

                timeval tval;

                tval.tv_sec  = m_interval;
                tval.tv_usec = 0;

                m_dht.m_timer.set_timer(this, &tval);
        }
}