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

#include "proxy.hpp"

#include <openssl/rand.h>

#include <boost/foreach.hpp>

namespace libcage {
        const time_t    proxy::register_timeout = 2;
        const time_t    proxy::get_timeout      = 10;


        size_t
        hash_value(const proxy::_id &i)
        {
                return i.id->hash_value();
        }

        proxy::proxy(const uint160_t &id, udphandler &udp, timer &t,
                     peers &p, dtun &dt, dht &dh) :
                m_id(id),
                m_udp(udp),
                m_timer(t),
                m_peers(p),
                m_dtun(dt),
                m_dht(dh),
                m_is_registered(false),
                m_is_registering(false),
                m_timer_register(*this)
        {
                RAND_pseudo_bytes((unsigned char*)&m_register_session,
                                  sizeof(m_register_session));
        }

        proxy::~proxy()
        {
                boost::unordered_map<uint32_t, gd_ptr>::iterator it;

                for (it = m_getdata.begin(); it != m_getdata.end(); ++it) {
                        m_timer.unset_timer(&it->second->timeout);
                }
        }

        void
        proxy::timer_register::operator() ()
        {
                m_proxy.m_is_registering  = false;
        }

        void
        proxy::register_func::operator() (std::vector<cageaddr> &nodes)
        {
                BOOST_FOREACH(cageaddr &addr, nodes) {
                        if (p_proxy->m_id == *addr.id)
                                continue;

                        msg_proxy_register reg;

                        memset(&reg, 0, sizeof(reg));

                        p_proxy->m_nonce = mrand48();

                        reg.session = htonl(p_proxy->m_register_session);
                        reg.nonce   = htonl(p_proxy->m_nonce);


                        // start timer
                        timeval tval;

                        tval.tv_sec  = proxy::register_timeout;
                        tval.tv_usec = 0;

                        p_proxy->m_timer.set_timer(&p_proxy->m_timer_register,
                                                   &tval);

                        send_msg(p_proxy->m_udp, &reg.hdr, sizeof(reg),
                                 type_proxy_register, addr, p_proxy->m_id);

                        break;
                }
        }

        void
        proxy::recv_register_reply(void *msg, sockaddr *from)
        {
                msg_proxy_register_reply *reply;
                cageaddr  addr;
                uint160_t dst;
                uint32_t  nonce;

                if (! m_is_registering)
                        return;

                reply = (msg_proxy_register_reply*)msg;

                dst.from_binary(reply->hdr.dst, sizeof(reply->hdr.dst));

                if (dst != m_id)
                        return;

                nonce = ntohl(reply->nonce);

                if (m_nonce != nonce)
                        return;

                addr = new_cageaddr(&reply->hdr, from);
                m_server = addr;

                m_is_registered = true;
                m_is_registering = false;
                m_timer.unset_timer(&m_timer_register);
        }

        void
        proxy::recv_register(void *msg, sockaddr *from)
        {
                msg_proxy_register *reg;
                cageaddr  addr;
                uint160_t dst;

                reg = (msg_proxy_register*)msg;

                dst.from_binary(reg->hdr.dst, sizeof(reg->hdr.dst));

                if (dst != m_id)
                        return;

                addr = new_cageaddr(&reg->hdr, from);


                boost::unordered_map<_id, _addr>::iterator it;
                _id i;

                i.id = addr.id;
                it = m_registered.find(i);

                if (it == m_registered.end()) {
                        _addr a;

                        a.session   = ntohl(reg->session);
                        a.addr      = addr;
                        a.recv_time = time(NULL);
                        a.last_registered = time(NULL);

                        m_dtun.register_node(addr.id, a.session);
                } else {
                        uint32_t session = ntohl(reg->session);

                        if (it->second.session == session) {
                                it->second.addr      = addr;
                                it->second.recv_time = time(NULL);
                        } else {
                                return;
                        }
                }

                msg_proxy_register_reply  reply;

                memset(&reply, 0, sizeof(reply));

                reply.nonce = reg->nonce;

                send_msg(m_udp, &reply.hdr, sizeof(reply),
                         type_proxy_register_reply, addr, m_id);
        }

        void
        proxy::store(const uint160_t &id, void *key, uint16_t keylen,
                     void *value, uint16_t valuelen, uint16_t ttl)
        {
                msg_proxy_store *store;
                char *p_value;
                char buf[1024 * 4];
                int  size;

                if (! m_is_registered)
                        return;

                size = sizeof(*store) - sizeof(store->data) + keylen + valuelen;

                if (size > (int)sizeof(buf))
                        return;

                store = (msg_proxy_store*)buf;

                memset(store, 0, size);

                id.to_binary(store->id, sizeof(store->id));

                store->keylen   = htons(keylen);
                store->valuelen = htons(valuelen);
                store->ttl      = htons(ttl);

                p_value = (char*)store->data;
                p_value += keylen;

                memcpy(store->data, key, keylen);
                memcpy(p_value, value, valuelen);

                send_msg(m_udp, &store->hdr, size, type_proxy_store,
                         m_server, m_id);
        }

        void
        proxy::recv_store(void *msg, int len, sockaddr *from)
        {
                msg_proxy_store *store;
                uint160_t dst;
                uint16_t  keylen;
                uint16_t  valuelen;
                int       size;

                store = (msg_proxy_store*)msg;

                keylen   = ntohs(store->keylen);
                valuelen = ntohs(store->valuelen);

                size = sizeof(*store) - sizeof(store->data)
                        + keylen + valuelen;

                if (size != len)
                        return;

                dst.from_binary(store->hdr.dst, sizeof(store->hdr.dst));
                if (dst != m_id)
                        return;


                uint160_t id;
                id_ptr    src(new uint160_t);
                _id       i;
                char     *p_value;

                src->from_binary(store->hdr.src, sizeof(store->hdr.src));

                i.id = src;
                if (m_registered.find(i) == m_registered.end())
                        return;


                id.from_binary(store->id, sizeof(store->id));

                p_value  = (char*)store->data;
                p_value += keylen;

                m_dht.store(id, store->data, keylen, p_value, valuelen,
                            ntohs(store->ttl));
        }

        void
        proxy::get_reply_func::operator() (bool result, void *buf, int len)
        {
                msg_proxy_get_reply *reply;
                int  size;
                char tmp[1024 * 4];

                reply = (msg_proxy_get_reply*)tmp;

                size = sizeof(*reply) - sizeof(reply->data);

                if (result) {
                        size += len;

                        memset(reply, 0, size);

                        reply->nonce = nonce;
                        reply->flag  = 1;

                        id->to_binary(reply->id, sizeof(reply->id));

                        memcpy(reply->data, buf, len);
                } else {
                        memset(reply, 0, size);

                        reply->nonce = nonce;
                        reply->flag  = 0;

                        id->to_binary(reply->id, sizeof(reply->id));
                }

                boost::unordered_map<_id, _addr>::iterator it;
                _id i;

                i.id = src;

                it = p_proxy->m_registered.find(i);
                if (it != p_proxy->m_registered.end()) {
                        send_msg(p_proxy->m_udp, &reply->hdr, size,
                                 type_proxy_get_reply,
                                 it->second.addr, p_proxy->m_id);
                }
        }

        void
        proxy::recv_get(void *msg, int len)
        {
                msg_proxy_get *getmsg;
                uint16_t keylen;
                int      size;
                id_ptr   src(new uint160_t);
                id_ptr   id(new uint160_t);
                _id      i;

                getmsg = (msg_proxy_get*)msg;


                src->from_binary(getmsg->hdr.src, sizeof(getmsg->hdr.src));
                i.id = src;
                if (m_registered.find(i) != m_registered.end())
                        return;

                keylen = ntohs(getmsg->keylen);

                size = sizeof(*getmsg) - sizeof(getmsg->key) +
                        keylen;

                if (size != len)
                        return;


                get_reply_func func;

                func.id      = id;
                func.src     = src;
                func.nonce   = getmsg->nonce;
                func.p_proxy = this;

                id->from_binary(getmsg->id, sizeof(getmsg->id));

                m_dht.find_value(*id, getmsg->key, keylen, func);
        }

        void
        proxy::timer_get::operator() ()
        {
                func(false, NULL, 0);

                p_proxy->m_getdata.erase(nonce);
        }

        void
        proxy::get(const uint160_t &id, void *key, uint16_t keylen,
                   callback_get func)
        {
                boost::shared_array<char> p_key;
                msg_proxy_get *msg;
                gd_ptr   data(new getdata);
                uint32_t nonce;
                int      size;
                char     buf[1024 * 4];

                msg = (msg_proxy_get*)buf;

                size = sizeof(*msg) - sizeof(msg->key) + keylen;
                if (size > (int)sizeof(buf)) {
                        func(false, NULL, 0);
                        return;
                }

                for (;;) {
                        nonce = mrand48();
                        if (m_getdata.find(nonce) == m_getdata.end())
                                break;
                }

                p_key = boost::shared_array<char>(new char[keylen]);
                memcpy(p_key.get(), key, keylen);

                data->keylen = keylen;
                data->func   = func;
                data->key    = p_key;

                data->timeout.p_proxy = this;
                data->timeout.nonce   = nonce;
                data->timeout.func    = func;

                m_getdata[nonce] = data;

                // start timer
                timeval tval;

                tval.tv_sec  = get_timeout;
                tval.tv_usec = 0;

                m_timer.set_timer(&data->timeout, &tval);

                // send get
                memset(msg, 0, size);

                msg->nonce  = htonl(nonce);
                msg->keylen = htons(keylen);

                id.to_binary(msg->id, sizeof(msg->id));
                memcpy(msg->key, key, keylen);

                send_msg(m_udp, &msg->hdr, size, type_proxy_get, m_server,
                         m_id);
        }

        void
        proxy::recv_get_reply(void *msg, int len)
        {
                msg_proxy_get_reply *reply;
                uint160_t dst;
                uint32_t  nonce;
                int       valuelen;

                reply = (msg_proxy_get_reply*)msg;

                dst.from_binary(reply->hdr.dst, sizeof(reply->hdr.dst));
                if (m_id != dst)
                        return;

                valuelen = len - (sizeof(*reply) - sizeof(reply->data));

                if (valuelen < 0)
                        return;

                nonce = ntohl(reply->nonce);

                
                boost::unordered_map<uint32_t, gd_ptr>::iterator it;

                it = m_getdata.find(nonce);
                if (it == m_getdata.end())
                        return;

                if (reply->flag > 0) {
                        it->second->func(false, NULL, 0);
                } else {
                        it->second->func(true, reply->data, valuelen);
                }

                m_timer.unset_timer(&it->second->timeout);

                m_getdata.erase(nonce);
        }

        void
        proxy::register_node()
        {
                if (m_is_registering)
                        return;

                m_is_registering = true;

                register_func func;

                func.p_proxy = this;

                m_dtun.find_node(m_id, func);
        }
}