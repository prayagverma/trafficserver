/** @file

  A brief file description

  @section license License

  Licensed to the Apache Software Foundation (ASF) under one
  or more contributor license agreements.  See the NOTICE file
  distributed with this work for additional information
  regarding copyright ownership.  The ASF licenses this file
  to you under the Apache License, Version 2.0 (the
  "License"); you may not use this file except in compliance
  with the License.  You may obtain a copy of the License at

      http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.
 */

#include "QUICAltConnectionManager.h"
#include "QUICConnectionTable.h"

static constexpr char V_DEBUG_TAG[] = "v_quic_alt_con";

#define QUICACMVDebug(fmt, ...) Debug(V_DEBUG_TAG, "[%s] " fmt, this->_qc->cids().data(), ##__VA_ARGS__)

QUICAltConnectionManager::QUICAltConnectionManager(QUICConnection *qc, QUICConnectionTable &ctable,
                                                   QUICConnectionId peer_initial_cid, uint32_t instance_id, uint8_t num_alt_con,
                                                   const QUICPreferredAddress preferred_address)
  : _qc(qc), _ctable(ctable), _instance_id(instance_id), _nids(num_alt_con)
{
  // Sequence number of the initial CID is 0
  this->_alt_quic_connection_ids_remote.push_back({0, peer_initial_cid, {}, {true}});

  // Sequence number of the preferred address is 1 if available
  if (preferred_address.is_available()) {
    this->_alt_quic_connection_ids_remote.push_back({1, preferred_address.cid(), preferred_address.token(), {false}});
  }

  this->_alt_quic_connection_ids_local = static_cast<AltConnectionInfo *>(ats_malloc(sizeof(AltConnectionInfo) * this->_nids));
}

QUICAltConnectionManager::QUICAltConnectionManager(QUICConnection *qc, QUICConnectionTable &ctable,
                                                   QUICConnectionId peer_initial_cid, uint32_t instance_id, uint8_t num_alt_con,
                                                   const IpEndpoint *preferred_endpoint)
  : _qc(qc), _ctable(ctable), _instance_id(instance_id), _nids(num_alt_con)
{
  // Sequence number of the initial CID is 0
  this->_alt_quic_connection_ids_remote.push_back({0, peer_initial_cid, {}, {true}});

  this->_alt_quic_connection_ids_local = static_cast<AltConnectionInfo *>(ats_malloc(sizeof(AltConnectionInfo) * this->_nids));
  this->_init_alt_connection_ids(preferred_endpoint);
}

QUICAltConnectionManager::~QUICAltConnectionManager()
{
  ats_free(this->_alt_quic_connection_ids_local);
  delete this->_preferred_address;
}

const QUICPreferredAddress *
QUICAltConnectionManager::preferred_address() const
{
  return this->_preferred_address;
}

std::vector<QUICFrameType>
QUICAltConnectionManager::interests()
{
  return {QUICFrameType::NEW_CONNECTION_ID, QUICFrameType::RETIRE_CONNECTION_ID};
}

QUICConnectionErrorUPtr
QUICAltConnectionManager::handle_frame(QUICEncryptionLevel level, const QUICFrame &frame)
{
  QUICConnectionErrorUPtr error = nullptr;

  switch (frame.type()) {
  case QUICFrameType::NEW_CONNECTION_ID:
    error = this->_register_remote_connection_id(static_cast<const QUICNewConnectionIdFrame &>(frame));
    break;
  case QUICFrameType::RETIRE_CONNECTION_ID:
    error = this->_retire_remote_connection_id(static_cast<const QUICRetireConnectionIdFrame &>(frame));
    break;
  default:
    QUICACMVDebug("Unexpected frame type: %02x", static_cast<unsigned int>(frame.type()));
    ink_assert(false);
    break;
  }

  return error;
}

QUICAltConnectionManager::AltConnectionInfo
QUICAltConnectionManager::_generate_next_alt_con_info()
{
  QUICConnectionId conn_id;
  conn_id.randomize();
  QUICStatelessResetToken token(conn_id, this->_instance_id);
  AltConnectionInfo aci = {++this->_alt_quic_connection_id_seq_num, conn_id, token, {false}};

  if (this->_qc->direction() == NET_VCONNECTION_IN) {
    this->_ctable.insert(conn_id, this->_qc);
  }

  if (is_debug_tag_set(V_DEBUG_TAG)) {
    char new_cid_str[QUICConnectionId::MAX_HEX_STR_LENGTH];
    conn_id.hex(new_cid_str, QUICConnectionId::MAX_HEX_STR_LENGTH);
    QUICACMVDebug("alt-cid=%s", new_cid_str);
  }

  return aci;
}

void
QUICAltConnectionManager::_init_alt_connection_ids(const IpEndpoint *preferred_endpoint)
{
  if (preferred_endpoint) {
    this->_alt_quic_connection_ids_local[0] = this->_generate_next_alt_con_info();
    // This alt cid will be advertised via Transport Parameter
    this->_alt_quic_connection_ids_local[0].advertised = true;

    this->_preferred_address = new QUICPreferredAddress(*preferred_endpoint, this->_alt_quic_connection_ids_local[0].id,
                                                        this->_alt_quic_connection_ids_local[0].token);
  }

  for (int i = (preferred_endpoint ? 1 : 0); i < this->_nids; ++i) {
    this->_alt_quic_connection_ids_local[i] = this->_generate_next_alt_con_info();
  }
  this->_need_advertise = true;
}

bool
QUICAltConnectionManager::_update_alt_connection_id(uint64_t chosen_seq_num)
{
  for (int i = 0; i < this->_nids; ++i) {
    if (_alt_quic_connection_ids_local[i].seq_num == chosen_seq_num) {
      _alt_quic_connection_ids_local[i] = this->_generate_next_alt_con_info();
      this->_need_advertise             = true;
      return true;
    }
  }

  // Seq 0 is special so it's not in the array
  if (chosen_seq_num == 0) {
    return true;
  }

  return false;
}

QUICConnectionErrorUPtr
QUICAltConnectionManager::_register_remote_connection_id(const QUICNewConnectionIdFrame &frame)
{
  QUICConnectionErrorUPtr error = nullptr;

  if (frame.connection_id() == QUICConnectionId::ZERO()) {
    error = std::make_unique<QUICConnectionError>(QUICTransErrorCode::PROTOCOL_VIOLATION, "received zero-length cid",
                                                  QUICFrameType::NEW_CONNECTION_ID);
  } else {
    this->_alt_quic_connection_ids_remote.push_back(
      {frame.sequence(), frame.connection_id(), frame.stateless_reset_token(), {false}});
  }

  return error;
}

QUICConnectionErrorUPtr
QUICAltConnectionManager::_retire_remote_connection_id(const QUICRetireConnectionIdFrame &frame)
{
  QUICConnectionErrorUPtr error = nullptr;

  if (!this->_update_alt_connection_id(frame.seq_num())) {
    error = std::make_unique<QUICConnectionError>(QUICTransErrorCode::PROTOCOL_VIOLATION, "received unused sequence number",
                                                  QUICFrameType::RETIRE_CONNECTION_ID);
  }
  return error;
}

bool
QUICAltConnectionManager::is_ready_to_migrate() const
{
  if (this->_alt_quic_connection_ids_remote.empty()) {
    return false;
  }

  for (auto &info : this->_alt_quic_connection_ids_remote) {
    if (!info.used) {
      return true;
    }
  }
  return false;
}

QUICConnectionId
QUICAltConnectionManager::migrate_to_alt_cid()
{
  if (this->_qc->direction() == NET_VCONNECTION_OUT) {
    this->_init_alt_connection_ids();
  }

  for (auto &info : this->_alt_quic_connection_ids_remote) {
    if (info.used) {
      continue;
    }
    info.used = true;
    return info.id;
  }

  ink_assert(!"Could not find CID available");
  return QUICConnectionId::ZERO();
}

bool
QUICAltConnectionManager::migrate_to(QUICConnectionId cid, QUICStatelessResetToken &new_reset_token)
{
  for (unsigned int i = 0; i < this->_nids; ++i) {
    AltConnectionInfo &info = this->_alt_quic_connection_ids_local[i];
    if (info.id == cid) {
      // Migrate connection
      new_reset_token = info.token;
      return true;
    }
  }
  return false;
}

void
QUICAltConnectionManager::drop_cid(QUICConnectionId cid)
{
  for (auto it = this->_alt_quic_connection_ids_remote.begin(); it != this->_alt_quic_connection_ids_remote.end(); ++it) {
    if (it->id == cid) {
      QUICACMVDebug("Dropping advertized CID %" PRIx32 " seq# %" PRIu64, it->id.h32(), it->seq_num);
      this->_retired_seq_nums.push(it->seq_num);
      this->_alt_quic_connection_ids_remote.erase(it);
      return;
    }
  }
}

void
QUICAltConnectionManager::invalidate_alt_connections()
{
  for (unsigned int i = 0; i < this->_nids; ++i) {
    this->_ctable.erase(this->_alt_quic_connection_ids_local[i].id, this->_qc);
  }
}

bool
QUICAltConnectionManager::will_generate_frame(QUICEncryptionLevel level)
{
  if (!this->_is_level_matched(level)) {
    return false;
  }

  return this->_need_advertise || !this->_retired_seq_nums.empty();
}

QUICFrameUPtr
QUICAltConnectionManager::generate_frame(QUICEncryptionLevel level, uint64_t connection_credit, uint16_t maximum_frame_size)
{
  QUICFrameUPtr frame = QUICFrameFactory::create_null_frame();
  if (!this->_is_level_matched(level)) {
    return frame;
  }

  if (this->_need_advertise) {
    int count = this->_nids;
    for (int i = 0; i < count; ++i) {
      if (!this->_alt_quic_connection_ids_local[i].advertised) {
        frame = QUICFrameFactory::create_new_connection_id_frame(this->_alt_quic_connection_ids_local[i].seq_num,
                                                                 this->_alt_quic_connection_ids_local[i].id,
                                                                 this->_alt_quic_connection_ids_local[i].token);

        if (frame && frame->size() > maximum_frame_size) {
          // Cancel generating frame
          frame = QUICFrameFactory::create_null_frame();
        } else {
          this->_alt_quic_connection_ids_local[i].advertised = true;
        }

        return frame;
      }
    }
    this->_need_advertise = false;
  }

  if (!this->_retired_seq_nums.empty()) {
    if (auto s = this->_retired_seq_nums.front()) {
      frame = QUICFrameFactory::create_retire_connection_id_frame(s);
      this->_retired_seq_nums.pop();
      return frame;
    }
  }

  return frame;
}
