/* Send blaming letters to @yrtimd */
#include <csnode/packstream.hpp>
#include <lib/system/allocators.hpp>
#include <lib/system/keys.hpp>

#include "network.hpp"
#include "transport.hpp"

enum RegFlags: uint8_t {
  UsingIPv6    = 1,
  RedirectIP   = 1 << 1,
  RedirectPort = 1 << 2
};

uint32_t CONNECTION_MAX_ATTEMPTS = 8;

namespace {
// Packets formation

void addMyOut(const Config& config, OPackStream& stream) {
  uint8_t regFlag = 0;
  if (!config.isSymmetric()) {
    if (config.getAddressEndpoint().ipSpecified) {
      regFlag|= RegFlags::RedirectIP;
      if (config.getAddressEndpoint().ip.is_v6())
        regFlag|= RegFlags::UsingIPv6;
    }

    regFlag|= RegFlags::RedirectPort;
  }
  else if (config.hasTwoSockets())
    regFlag|= RegFlags::RedirectPort;

  uint8_t* flagChar = stream.getCurrPtr() - 1;
  *flagChar|= regFlag;

  if (!config.isSymmetric()) {
    if (config.getAddressEndpoint().ipSpecified)
      stream << config.getAddressEndpoint().ip;

    stream << config.getAddressEndpoint().port;
  }
  else if (config.hasTwoSockets())
    stream << config.getInputEndpoint().port;
}

void formRegPack(const Config& config,
                 OPackStream& stream,
                 uint64_t** regPackConnId,
                 const PublicKey& pk) {
  stream.init(BaseFlags::NetworkMsg);

  stream <<
    NetworkCommand::Registration <<
    NODE_VERSION <<
    (uint8_t)0;

  addMyOut(config, stream);
  *regPackConnId = (uint64_t*)stream.getCurrPtr();

  stream <<
    (ConnectionId)0 <<
    pk;
}

void formSSConnectPack(const Config& config, OPackStream& stream, const PublicKey& pk) {
  stream.init(BaseFlags::NetworkMsg);
  stream << NetworkCommand::SSRegistration
         << NODE_VERSION
         << (uint8_t)(config.getNodeType() == NodeType::Router);

  addMyOut(config, stream);

  stream << pk;
}
}

void Transport::run(const Config& config) {
  acceptRegistrations_ = config.getNodeType() == NodeType::Router;

  formRegPack(config, oPackStream_, &regPackConnId_, myPublicKey_);
  regPack_ = *(oPackStream_.getPackets());
  oPackStream_.clear();

  if (config.getBootstrapType() == BootstrapType::IpList) {
    uint32_t cntr = 0;
    for (auto& ep : config.getIpList()) {
      if (cntr++ == Neighbourhood::MaxConnections) {
        LOG_WARN("Connections limit reached");
        break;
      }

      LOG_EVENT("Creating connection to " << ep.ip);
      nh_.establishConnection(net_->resolve(ep));
    }
  }
  else {
    // Connect to SS logic
    LOG_EVENT("Connecting to Singal Server on " << ssEp_);
    ssEp_ = net_->resolve(config.getSignalServerEndpoint());

    formSSConnectPack(config, oPackStream_, myPublicKey_);
    ssStatus_ = SSBootstrapStatus::Requested;
    net_->sendDirect(*(oPackStream_.getPackets()),
                     ssEp_);
  }

  // Okay, now let's get to business
  oPackStream_.init(BaseFlags::NetworkMsg);
  oPackStream_ << NetworkCommand::Ping;
  auto pingPack = *(oPackStream_.getPackets());

  uint32_t ctr = 0;
  for (;;) {
    ++ctr;
    bool resendPacks = ctr % 3 == 0;
    bool sendPing = ctr % 10 == 0;
    bool checkConnections = ctr % 30 == 0;

    if (checkConnections) {
      nh_.checkPending();
      nh_.checkSilent();
    }

    if (resendPacks) {
      for (auto& c : sendPacks_)
        sendBroadcast(&c);
    }

    if (sendPing)
      sendBroadcast(&pingPack);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
}

template <>
inline uint16_t getHashIndex(const ip::udp::endpoint& ep) {
  uint16_t result = ep.port();

  if (ep.protocol() == ip::udp::v4()) {
    uint32_t addr = ep.address().to_v4().to_uint();
    result^= *(uint16_t*)&addr;
    result^= *((uint16_t*)&addr + 1);
  }
  else {
    auto bytes = ep.address().to_v6().to_bytes();
    auto ptr = (uint8_t*)&result;
    auto bytesPtr = bytes.data();
    for (uint32_t i = 0; i < 8; ++i) *ptr^= *(bytesPtr++);
    ++ptr;
    for (uint32_t i = 8; i < 16; ++i) *ptr^= *(bytesPtr++);
  }

  return result;
}

RemoteNodePtr Transport::getPackSenderEntry(const ip::udp::endpoint& ep) {
  auto& rn = remoteNodesMap_.tryStore(ep);

  if (!rn)  // Newcomer
    rn = remoteNodes_.emplace();

  rn->packets.fetch_add(1, std::memory_order_relaxed);
  return rn;
}

// Processing network packages

void Transport::processNetworkTask(const TaskPtr<IPacMan>& task,
                                   RemoteNodePtr& sender) {
  iPackStream_.init(task->pack.getMsgData(),
                    task->pack.getMsgSize());

  NetworkCommand cmd;
  iPackStream_ >> cmd;

  if (!iPackStream_.good())
    return sender->addStrike();

  bool result = true;
  switch (cmd) {
  case NetworkCommand::Registration:
    result = gotRegistrationRequest(task, sender);
    break;
  case NetworkCommand::ConfirmationRequest: break;
  case NetworkCommand::ConfirmationResponse: break;
  case NetworkCommand::RegistrationConfirmed:
    result = gotRegistrationConfirmation(task, sender);
    break;
  case NetworkCommand::RegistrationRefused:
    result = gotRegistrationRefusal(task, sender);
    break;
  case NetworkCommand::Ping:
    LOG_EVENT("Ping from " << task->sender);
    break;
  case NetworkCommand::SSRegistration:
    if (task->sender != ssEp_) { result = false; break; }
    gotSSRegistration(task);
    break;
  case NetworkCommand::SSFirstRound:
    if (task->sender != ssEp_) { result = false; break; }
    gotSSDispatch(task);
    break;
  case NetworkCommand::SSRegistrationRefused:
    if (task->sender != ssEp_) { result = false; break; }
    gotSSRefusal(task);
    break;
  default:
    result = false;
    LOG_WARN("Unexpected network command");
  }

  if (!result)
    sender->addStrike();
}

bool Transport::parseSSSignal(const TaskPtr<IPacMan>& task) {
  iPackStream_.init(task->pack.getMsgData(), task->pack.getMsgSize());
  iPackStream_.safeSkip<uint8_t>(1);

  auto trStart = iPackStream_.getCurrPtr();
  iPackStream_.safeSkip<uint32_t>();

  uint8_t numConf;
  iPackStream_ >> numConf;
  if (!iPackStream_.good()) return false;

  iPackStream_.safeSkip<PublicKey>(numConf + 1);

  auto trFinish = iPackStream_.getCurrPtr();
  node_->getRoundTable(trStart, (trFinish - trStart));

  uint8_t numCirc;
  iPackStream_ >> numCirc;
  if (!iPackStream_.good()) return false;

  for (uint8_t i = 0; i < numCirc; ++i) {
    EndpointData ep;
    ep.ipSpecified = true;

    iPackStream_ >> ep.ip >> ep.port;
    if (!iPackStream_.good()) return false;

    nh_.establishConnection(net_->resolve(ep));

    iPackStream_.safeSkip<PublicKey>();
    if (!iPackStream_.good()) return false;
  }

  ssStatus_ = SSBootstrapStatus::Complete;

  return true;
}

void Transport::processNodeMessage(const Message& msg) {
  dispatchNodeMessage(msg.getFirstPack(),
                      msg.getFullData(),
                      msg.getFullSize());
}

void Transport::processNodeMessage(const Packet& pack) {
  dispatchNodeMessage(pack, pack.getMsgData(), pack.getMsgSize());
}

void Transport::dispatchNodeMessage(const Packet& firstPack,
                                    const uint8_t* data,
                                    size_t size) {
  if (!size) {
    LOG_ERROR("Bad packet size, why is it zero?");
    return;
  }

  ++data;
  --size;

  switch(firstPack.getType()) {
  case MsgTypes::RoundTable:
    return node_->getRoundTable(data, size);
  case MsgTypes::Transactions:
    return node_->getTransaction(data, size);
  case MsgTypes::FirstTransaction:
    return node_->getFirstTransaction(data, size);
  case MsgTypes::TransactionList:
    return node_->getTransactionsList(data, size);
  case MsgTypes::ConsVector:
    return node_->getVector(data, size, firstPack.getSender());
  case MsgTypes::ConsMatrix:
    return node_->getMatrix(data, size, firstPack.getSender());
  case MsgTypes::NewBlock:
    return node_->getBlock(data, size, firstPack.getSender());
  case MsgTypes::BlockHash:
    return node_->getHash(data, size, firstPack.getSender());
  default:
    LOG_ERROR("Unknown type");
    break;
  }
}

void Transport::addTask(Packet* pack, const uint32_t packNum) {
  auto end = pack + packNum;
  for (auto ptr = pack; ptr != end; ++ptr) {
    sendBroadcast(ptr);
    {
      SpinLock l(sendPacksFlag_);
      sendPacks_.emplace(*ptr);
    }
  }
}

void Transport::clearTasks() {
  SpinLock l(sendPacksFlag_);
  sendPacks_.clear();
}

/* Sending network tasks */
void Transport::sendRegistrationRequest(Connection& conn) {
  Packet req(netPacksAllocator_.allocateNext(regPack_.size()));
  *regPackConnId_ = conn.id;
  memcpy(req.data(), regPack_.data(), regPack_.size());

  ++(conn.attempts);
  sendDirect(&req, conn);
}

void Transport::sendRegistrationConfirmation(const Connection& conn) {
  LOG_EVENT("Confirming registration with " << conn.in);

  oPackStream_.init(BaseFlags::NetworkMsg);
  oPackStream_ << NetworkCommand::RegistrationConfirmed << conn.id << myPublicKey_;

  sendDirect(oPackStream_.getPackets(), conn);
  oPackStream_.clear();
}

void Transport::sendRegistrationRefusal(const Connection& conn,
                                        const RegistrationRefuseReasons reason) {
    LOG_EVENT("Refusing registration with " << conn.in);

  oPackStream_.init(BaseFlags::NetworkMsg);
  oPackStream_ << NetworkCommand::RegistrationRefused << conn.id << reason;

  sendDirect(oPackStream_.getPackets(), conn);
  oPackStream_.clear();
}

// Requests processing

bool Transport::gotRegistrationRequest(const TaskPtr<IPacMan>& task,
                                       RemoteNodePtr& sender) {
  LOG_EVENT("Got registration request from " << task->sender);

  NodeVersion vers;
  iPackStream_ >> vers;
  if (!iPackStream_.good()) return false;

  Connection conn;
  conn.in = task->sender;
  auto& flags = iPackStream_.peek<uint8_t>();

  if (flags & RegFlags::RedirectIP) {
    boost::asio::ip::address addr;
    iPackStream_ >> addr;

    conn.out.address(addr);
    conn.specialOut = true;
  }
  else {
    conn.specialOut = false;
    iPackStream_.skip<uint8_t>();
  }

  if (flags & RegFlags::RedirectPort) {
    Port port;
    iPackStream_ >> port;

    if (!conn.specialOut) {
      conn.specialOut = true;
      conn.out.address(task->sender.address());
    }

    conn.out.port(port);
  }
  else if (conn.specialOut)
    conn.out.port(task->sender.port());

  if (vers != NODE_VERSION) {
    sendRegistrationRefusal(conn, RegistrationRefuseReasons::BadClientVersion);
    return true;
  }

  iPackStream_ >> conn.id;
  iPackStream_ >> conn.key;

  if (!iPackStream_.good() || !iPackStream_.end())
    return false;

  nh_.gotRegistration(std::move(conn), sender);
  return true;
}

bool Transport::gotRegistrationConfirmation(const TaskPtr<IPacMan>& task,
                                            RemoteNodePtr& sender) {
  ConnectionId cId;
  PublicKey key;
  iPackStream_ >> cId >> key;

  if (!iPackStream_.good())
    return false;

  nh_.gotConfirmation(cId, task->sender, key, sender);
  return true;
}

bool Transport::gotRegistrationRefusal(const TaskPtr<IPacMan>& task,
                                       RemoteNodePtr&) {
  RegistrationRefuseReasons reason;
  Connection::Id id;
  iPackStream_ >> id >> reason;

  if (!iPackStream_.good() || !iPackStream_.end())
    return false;

  nh_.gotRefusal(id);

  LOG_EVENT("Registration to " << task->sender << " refused. Reason: " << (int)reason);

  return true;
}

bool Transport::gotSSRegistration(const TaskPtr<IPacMan>& task) {
  if (ssStatus_ != SSBootstrapStatus::Requested) {
    LOG_WARN("Unexpected Signal Server response");
    return false;
  }

  LOG_EVENT("Connection to the Signal Server has been established");
  if (task->pack.getMsgSize() > 2) {
    if (!parseSSSignal(task))
      LOG_WARN("Bad Signal Server response");
  }
  else
    ssStatus_ = SSBootstrapStatus::RegisteredWait;

  return true;
}

bool Transport::gotSSDispatch(const TaskPtr<IPacMan>& task) {
  if (ssStatus_ != SSBootstrapStatus::RegisteredWait)
    LOG_WARN("Unexpected Signal Server response");

  if (!parseSSSignal(task))
    LOG_WARN("Bad Signal Server response");

  return true;
}

bool Transport::gotSSRefusal(const TaskPtr<IPacMan>& task) {
  uint16_t expectedVersion;
  iPackStream_ >> expectedVersion;

  LOG_ERROR("The Signal Server has refused the registration due to your bad client version. The expected version is " << expectedVersion);

  return true;
}
