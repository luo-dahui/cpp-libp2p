/**
 * Copyright Soramitsu Co., Ltd. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <functional>
#include <libp2p/multi/uvarint.hpp>
#include <libp2p/protocol/kademlia/message.hpp>

#include <generated/protocol/kademlia/protobuf/kademlia.pb.h>

namespace libp2p::protocol::kademlia {

  using ConnStatus = network::ConnectionManager::Connectedness;

  namespace {

    inline void assign_blob(std::vector<uint8_t> &dst, const std::string &src) {
      auto sz = src.size();
      if (sz == 0) {
        dst.clear();
      } else {
        dst.reserve(sz);
        dst.assign(src.begin(), src.end());
      }
    }

    boost::optional<Message::Peer> assign_peer(const pb::Message_Peer &src) {
      if (static_cast<ConnStatus>(src.connection())
          > ConnStatus::CAN_NOT_CONNECT) {
        // TODO(artem): log
        return boost::none;
      }

      auto peer_id_res = PeerId::fromBytes(
          gsl::span<uint8_t>((uint8_t *)src.id().data(), src.id().size()));
      if (!peer_id_res) {
        // TODO(artem): log
        return boost::none;
      }

      std::vector<multi::Multiaddress> addresses;
      for (const auto &addr : src.addrs()) {
        auto res = multi::Multiaddress::create(
            gsl::span<const uint8_t>((const uint8_t *)addr.data(), addr.size()));
        if (!res) {
          // TODO(artem): log
          return boost::none;
        }
        addresses.push_back(res.value());
      }

      return Message::Peer{PeerInfo{peer_id_res.value(), std::move(addresses)},
                           ConnStatus(src.connection())};
    }

    template <class PbContainer>
    bool assign_peers(boost::optional<Message::Peers> &dst,
                      const PbContainer &src) {
      if (!src.empty()) {
        dst = Message::Peers{};
        Message::Peers &v = dst.value();
        v.reserve(src.size());
        for (const auto &p : src) {
          auto res = assign_peer(p);
          if (!res) {
            // TODO(artem): log
            return false;
          }
          v.push_back(std::move(res.value()));
        }
      }
      return true;
    }

    template <class PbContainer>
    boost::optional<Message::Record> assign_record(const PbContainer &src) {
      auto ca_res = Key::fromWire(src.key());
      if (!ca_res) {
        return {};
      }
      boost::optional<Message::Record> record = Message::Record{
          std::move(ca_res.value()), Value(), src.timereceived()};
      assign_blob(record.value().value, src.value());
      return record;
    }

  }  // namespace

  void Message::clear() {
    type = Type::kPing;
    key.clear();
    record.reset();
    closer_peers.reset();
    provider_peers.reset();
  }

  bool Message::deserialize(const void *data, size_t sz) {
    clear();
    pb::Message pb_msg;
    if (!pb_msg.ParseFromArray(data, sz)) {
      return false;
    }
    type = static_cast<Type>(pb_msg.type());
    if (type > Type::kPing) {
      return false;
    }
    assign_blob(key, pb_msg.key());
    if (pb_msg.has_record()) {
      record = assign_record(pb_msg.record());
      if (!record) {
        return false;
      }
    }
    if (!assign_peers(closer_peers, pb_msg.closerpeers())) {
      return false;
    }
    return assign_peers(provider_peers, pb_msg.providerpeers());
  }

  bool Message::serialize(std::vector<uint8_t> &buffer) const {
    pb::Message pb_msg;
    pb_msg.set_type(pb::Message_MessageType(type));
    pb_msg.set_key(key.data(), key.size());
    if (record) {
      const Record &rec_src = record.value();
      pb::Record rec;
      rec.set_key(rec_src.key.data.data(), rec_src.key.data.size());
      rec.set_value(rec_src.value.data(), rec_src.value.size());
      rec.set_timereceived(rec_src.time_received);
      *pb_msg.mutable_record() = std::move(rec);
    }
    if (closer_peers) {
      for (const auto &p : closer_peers.value()) {
        pb::Message_Peer *pb_peer = pb_msg.add_closerpeers();
	      auto& pid_v = p.info.id.toVector();
        pb_peer->set_id(std::string(pid_v.begin(), pid_v.end()));
        for (const auto &addr : p.info.addresses) {
	        auto& bytes = addr.getBytesAddress();
          pb_peer->add_addrs(std::string(bytes.begin(), bytes.end()));
        }
        pb_peer->set_connection(pb::Message_ConnectionType(p.conn_status));
      }
    }
    if (provider_peers) {
      for (const auto &p : provider_peers.value()) {
        pb::Message_Peer *pb_peer = pb_msg.add_providerpeers();
	      auto& pid_v = p.info.id.toVector();
	      pb_peer->set_id(std::string(pid_v.begin(), pid_v.end()));
        for (const auto &addr : p.info.addresses) {
	        auto& bytes = addr.getBytesAddress();
	        pb_peer->add_addrs(std::string(bytes.begin(), bytes.end()));
        }
        pb_peer->set_connection(pb::Message_ConnectionType(p.conn_status));
      }
    }
    size_t msg_sz = pb_msg.ByteSizeLong();
    auto varint_len = multi::UVarint{msg_sz};
    auto varint_vec = varint_len.toVector();
    size_t prefix_sz = varint_vec.size();
    buffer.resize(prefix_sz + msg_sz);
    memcpy(buffer.data(), varint_vec.data(), prefix_sz);
    return pb_msg.SerializeToArray(buffer.data() + prefix_sz,  // NOLINT
                                   msg_sz);
  }

  void Message::selfAnnounce(PeerInfo self) {
    closer_peers = Message::Peers{
        {Message::Peer{std::move(self), Message::Connectedness::CAN_CONNECT}}};
  }

  Message createPutValueRequest(const Key &key, const Value &value) {
    Message msg;
    msg.type = Message::Type::kPutValue;
    msg.record.emplace(Message::Record{key, value, "timestamp"});
    return msg;
  }

  Message createGetValueRequest(const Key &key,
                                boost::optional<PeerInfo> self_announce) {
    Message msg;
    msg.type = Message::Type::kGetValue;
    msg.key = key.data;
    if (self_announce) {
      msg.selfAnnounce(std::move(self_announce.value()));
    }
    return msg;
  }

  Message createAddProviderRequest(PeerInfo self, const Key &key) {
    Message msg;
    msg.type = Message::Type::kAddProvider;
    msg.key = key.data;
    msg.provider_peers = Message::Peers{
        {Message::Peer{std::move(self), Message::Connectedness::CAN_CONNECT}}};
    return msg;
  }

  Message createGetProvidersRequest(const Key &key,
                                    boost::optional<PeerInfo> self_announce) {
    Message msg;
    msg.type = Message::Type::kGetProviders;
    msg.key = key.data;
    if (self_announce) {
      msg.selfAnnounce(std::move(self_announce.value()));
    }
    return msg;
  }

  Message createFindNodeRequest(const PeerId &node,
                                boost::optional<PeerInfo> self_announce) {
    Message msg;
    msg.type = Message::Type::kFindNode;
    msg.key = node.toVector();
    if (self_announce) {
      msg.selfAnnounce(std::move(self_announce.value()));
    }
    return msg;
  }

}  // namespace libp2p::protocol::kademlia
