#include <smartcontracts.hpp>
#include <solvercontext.hpp>

#include <ContractExecutor.h>
#include <base58.h>
#include <cscrypto/cryptoconstants.hpp>
#include <csdb/currency.hpp>
#include <csnode/datastream.hpp>
#include <lib/system/logger.hpp>
#include <csnode/fee.hpp>
#include <functional>
#include <memory>
#include <optional>
#include <sstream>

namespace {
    const char* kLogPrefix = "Smart: ";

    inline void print(std::ostream& os, const ::general::Variant& var) {
        os << "Variant(";
        bool print_default = false;
        if (var.__isset.v_string) {
            os << var.v_string;
        }
        else if (var.__isset.v_null) {
            os << "Null";
        }
        else if (var.__isset.v_boolean) {
            os << var.v_boolean;
        }
        else if (var.__isset.v_boolean_box) {
            os << var.v_boolean_box;
        }
        else if (var.__isset.v_array) {
            os << "Array";
        }
        else if (var.__isset.v_object) {
            os << "Object";
        }
        else if (var.__isset.v_void) {
            os << "Void";
        }
        else if (var.__isset.v_list) {
            os << "List";
        }
        else if (var.__isset.v_set) {
            os << "Set";
        }
        else if (var.__isset.v_map) {
            os << "Map";
        }
        else if (var.__isset.v_int) {
            os << var.v_int;
        }
        else if (var.__isset.v_int_box) {
            os << var.v_int_box;
        }
        else if (var.__isset.v_byte) {
            os << (unsigned int)var.v_byte;
        }
        else if (var.__isset.v_byte_box) {
            os << (unsigned int)var.v_byte_box;
        }
        else if (var.__isset.v_short) {
            os << var.v_short;
        }
        else if (var.__isset.v_short_box) {
            os << var.v_short_box;
        }
        else if (var.__isset.v_long) {
            os << var.v_long;
        }
        else if (var.__isset.v_long_box) {
            os << var.v_long_box;
        }
        else if (var.__isset.v_float) {
            os << var.v_float;
        }
        else if (var.__isset.v_float_box) {
            os << var.v_float_box;
        }
        else if (var.__isset.v_double) {
            os << var.v_double;
        }
        else if (var.__isset.v_double_box) {
            os << var.v_double_box;
        }
        else if (var.__isset.v_big_decimal) {
            os << var.v_big_decimal;
        }
        else if (var.__isset.v_byte_array) {
            os << "byte[" << var.v_byte_array.size() << ']';
        }
        else {
            /* other variant types are shown by default */
            print_default = true;
        }
        os << ')';

        if (print_default) {
            os << ": ";
            var.printTo(os);
        }
    }

    // serializes val passed to special transaction user field new_state::RetVal
    inline void set_return_value(csdb::Transaction& new_state_transaction, const ::general::Variant& val) {
        new_state_transaction.add_user_field(cs::trx_uf::new_state::RetVal, serialize(val));
    }

    inline void set_return_value(csdb::Transaction& new_state_transaction, uint8_t val) {
        ::general::Variant variant;
        variant.__set_v_byte(val);
        set_return_value(new_state_transaction, variant);
    }

}

namespace cs {

csdb::UserField SmartContractRef::to_user_field() const {
    cs::Bytes data;
    cs::DataStream stream(data);
    stream << hash << sequence << transaction;
    return csdb::UserField(stream.convert<std::string>());
}

void SmartContractRef::from_user_field(const csdb::UserField& fld) {
    std::string data = fld.value<std::string>();
    cs::DataStream stream(data.c_str(), data.size());
    stream >> hash >> sequence >> transaction;
    if (!stream.isValid() || stream.isAvailable(1)) {
        cserror() << "SmartCotractRef: read from malformed user field, abort!";
        hash = csdb::PoolHash{};
        sequence = std::numeric_limits<decltype(sequence)>().max();
        transaction = std::numeric_limits<decltype(transaction)>().max();
    }
}

void SmartContracts::QueueItem::add(const SmartContractRef& ref_contract, csdb::Transaction tr_start) {
    csdb::Amount tr_start_fee = csdb::Amount(tr_start.counted_fee().to_double());
    // TODO: here new_state_fee prediction may be calculated, currently it is equal to starter fee
    csdb::Amount new_state_fee = tr_start_fee;
    // apply starter fee consumed
    csdb::Amount avail_fee = csdb::Amount(tr_start.max_fee().to_double()) - tr_start_fee - new_state_fee;
    //consumed_fee = 0;
    auto& execution = executions.emplace_back(ExecutionItem{ ref_contract, avail_fee, new_state_fee, csdb::Amount{ 0 }, {}, {} });

    if (SmartContracts::is_executable(tr_start)) {
        const csdb::UserField fld = tr_start.user_field(trx_uf::start::Methods);  // start::Methods == deploy::Code, so does not matter what type of executable is
        if (fld.is_valid()) {
            std::string data = fld.value<std::string>();
            if (!data.empty()) {
                auto invoke = deserialize<api::SmartContractInvocation>(std::move(data));
                if (!invoke.usedContracts.empty()) {
                    for (const auto item : invoke.usedContracts) {
                        const csdb::Address addr = BlockChain::getAddressFromKey(item);
                        if (addr.is_valid()) {
                            execution.uses.push_back(addr);
                        }
                    }
                }
            }
        }
    }
    // reserve new_state fee for every using contract also
    if (!execution.uses.empty()) {
        for (const auto& it : execution.uses) {
            csunused(it);
            execution.avail_fee -= new_state_fee;
        }
    }
}

/*explicit*/
SmartContracts::SmartContracts(BlockChain& blockchain, CallsQueueScheduler& calls_queue_scheduler)
: scheduler(calls_queue_scheduler)
, force_execution(false)
, bc(blockchain)
, execution_allowed(true)
{
    // signals subscription (MUST occur AFTER the BlockChains has already subscribed to storage)

    // as event receiver:
    cs::Connector::connect(&bc.storeBlockEvent, this, &SmartContracts::on_store_block);
    cs::Connector::connect(&bc.readBlockEvent(), this, &SmartContracts::on_read_block);
    // as event source:
    cs::Connector::connect(&signal_payable_invoke, &bc, &BlockChain::onPayableContractReplenish);
    cs::Connector::connect(&signal_payable_timeout, &bc, &BlockChain::onPayableContractTimeout);
    cs::Connector::connect(&signal_emitted_accepted, &bc, &BlockChain::onContractEmittedAccepted);
}

SmartContracts::~SmartContracts() = default;

void SmartContracts::init(const cs::PublicKey& id, Node* node) {
    cs::Lock lock(public_access_lock);

    cs::Connector::connect(&node->gotRejectedContracts, this, &SmartContracts::on_reject);

    pnode = node;
    auto connector_ptr = pnode->getConnector();
    if (connector_ptr != nullptr) {
        exec_handler_ptr = connector_ptr->apiExecHandler();
    }
    node_id = id;
    force_execution = pnode->alwaysExecuteContracts();

    // currently, blockchain is read in such manner that does not require absolute/optimized consolidation post-factum
    // anyway this tested code may become useful in future

    size_t cnt = known_contracts.size();
    // consolidate contract states addressed by public keys with those addressed by wallet ids
    auto pred = [](const auto& item) { return item.first.is_wallet_id(); };
    auto it = std::find_if(known_contracts.cbegin(), known_contracts.cend(), pred);
    while (it != known_contracts.cend()) {
        // non-absolute address item is always newer then absolute one:
        csdb::Address abs_addr = absolute_address(it->first);
        if (abs_addr.is_valid()) {
            const StateItem& opt_out = it->second;
            if (!opt_out.state.empty()) {
                StateItem& updated = known_contracts[abs_addr];
                if (opt_out.ref_deploy.is_valid()) {
                    if (updated.ref_deploy.is_valid()) {
                        cswarning() << kLogPrefix << "contract deploy is overwritten by subsequent deploy of the same contract";
                    }
                    updated.ref_deploy = opt_out.ref_deploy;
                    updated.state = opt_out.state;
                }
                if (opt_out.ref_execute.is_valid()) {
                    updated.ref_execute = opt_out.ref_execute;
                    updated.state = opt_out.state;
                }
            }
            else {
                cswarning() << kLogPrefix << "empty state stored in contracts states table";
            }
        }
        known_contracts.erase(it);
        it = std::find_if(known_contracts.begin(), known_contracts.end(), pred);
    }

    // validate contract states
    for (const auto& item : known_contracts) {
        const StateItem& val = item.second;
        if (val.state.empty()) {
            cswarning() << kLogPrefix << "completely unsuccessful contract found, neither deployed, nor executed";
        }
        if (!val.ref_deploy.is_valid()) {
            cswarning() << kLogPrefix << "unsuccessfully deployed contract found";
        }
    }

    size_t new_cnt = known_contracts.size();
    cslog() << kLogPrefix << "" << new_cnt << " smart contract states loaded";
    if (cnt > new_cnt) {
        cslog() << kLogPrefix << "" << cnt - new_cnt << " smart contract state(s) is/are optimizied out";
    }
}

/*static*/
std::string SmartContracts::get_error_message(uint8_t code) {
    using namespace cs::error;
    switch (code) {
    case TimeExpired:
        return "timeout during operation";
    case OutOfFunds:
        return "insufficient funds to complete operation";
    case StdException:
        return "connection error while executing contract";
    case Exception:
        return "common error while executing contract";
    case UnpayableReplenish:
        return "replenished contract does not implement payable()";
    case ConsensusRejected:
        return "the trusted consensus have rejected new_state (or emitted transactions)";
    case ExecuteTransaction:
        return "common error in executor";
    case InternalBug:
        return "internal bug in node detected";
    case ExecutionError:
        return "executor is disconnected or unavailable, or incompatible";
    }
    std::ostringstream os;
    os << "Error code " << (unsigned int)code;
    return os.str();
}

/*static*/
bool SmartContracts::is_smart_contract(const csdb::Transaction& tr) {
    if (!tr.is_valid()) {
        return false;
    }
    // to contain smart contract trx must contain either FLD[0] (deploy, start) or FLD[-2] (new_state), both of type
    // "String":
    csdb::UserField f = tr.user_field(trx_uf::deploy::Code);
    if (!f.is_valid()) {
        f = tr.user_field(trx_uf::new_state::Value);
    }
    return f.is_valid() && f.type() == csdb::UserField::Type::String;
}

/*static*/
bool SmartContracts::is_executable(const csdb::Transaction& tr) {
    return SmartContracts::is_smart_contract(tr) && !SmartContracts::is_new_state(tr);
}

/*static*/
bool SmartContracts::is_deploy(const csdb::Transaction& tr) {
    if (!SmartContracts::is_executable(tr)) {
        return false;
    }

    using namespace cs::trx_uf;
    csdb::UserField uf = tr.user_field(deploy::Code);
    if (!uf.is_valid()) {
        return false;
    }

    const auto invoke = deserialize<api::SmartContractInvocation>(uf.value<std::string>());
    // deploy ~ start but method in invoke info is empty
    return invoke.method.empty();
}

/*static*/
bool SmartContracts::is_start(const csdb::Transaction& tr) {
    return SmartContracts::is_executable(tr) && !SmartContracts::is_deploy(tr);
}

/*static*/
bool SmartContracts::is_new_state(const csdb::Transaction& tr) {
    // must contain user field new_state::Value and new_state::RefStart
    using namespace cs::trx_uf;
    // test user_field[RefStart] helps filter out ancient smart contracts:
    return (tr.user_field(new_state::Value).type() == csdb::UserField::Type::String && tr.user_field(new_state::RefStart).type() == csdb::UserField::Type::String);
}

/* static */
/* Assuming deployer.is_public_key() */
csdb::Address SmartContracts::get_valid_smart_address(const csdb::Address& deployer, const uint64_t trId, const api::SmartContractDeploy& data) {
    static_assert(cscrypto::kHashSize <= cscrypto::kPublicKeySize);
    const uint8_t kInnerIdSize = 6;

    std::vector<cscrypto::Byte> strToHash;
    std::string byteCode{};
    if (!data.byteCodeObjects.empty()) {
        for (auto& curr_byteCode : data.byteCodeObjects) {
            byteCode += curr_byteCode.byteCode;
        }
    }
    strToHash.reserve(cscrypto::kPublicKeySize + kInnerIdSize + byteCode.size());

    const auto dPk = deployer.public_key();
    const auto idPtr = reinterpret_cast<const cscrypto::Byte*>(&trId);

    std::copy(dPk.begin(), dPk.end(), std::back_inserter(strToHash));
    std::copy(idPtr, idPtr + kInnerIdSize, std::back_inserter(strToHash));
    std::copy(byteCode.begin(), byteCode.end(), std::back_inserter(strToHash));

    cscrypto::Hash hash = cscrypto::calculateHash(strToHash.data(), strToHash.size());
    cscrypto::PublicKey res;
    res.fill(0);
    std::copy(hash.data(), hash.data() + cscrypto::kHashSize, res.data());

    return csdb::Address::from_public_key(res);
}

/*static*/
csdb::Transaction SmartContracts::get_transaction(BlockChain& storage, const SmartContractRef& contract) {
    csdb::Pool block = storage.loadBlock(contract.sequence);
    if (!block.is_valid()) {
        return csdb::Transaction{};
    }
    if (contract.transaction >= block.transactions_count()) {
        return csdb::Transaction{};
    }
    return block.transactions().at(contract.transaction);
}

std::optional<api::SmartContractInvocation> SmartContracts::find_deploy_info(const csdb::Address& abs_addr) const {
    using namespace trx_uf;
    const auto item = known_contracts.find(abs_addr);
    if (item != known_contracts.cend()) {
        const StateItem& val = item->second;
        if (val.ref_deploy.is_valid()) {
            csdb::Transaction tr_deploy = get_transaction(val.ref_deploy);
            if (tr_deploy.is_valid()) {
                csdb::UserField fld = tr_deploy.user_field(deploy::Code);
                if (fld.is_valid()) {
                    std::string data = fld.value<std::string>();
                    if (!data.empty()) {
                        return std::make_optional(deserialize<api::SmartContractInvocation>(std::move(data)));
                    }
                }
            }
        }
    }
    return std::nullopt;
}

bool SmartContracts::is_replenish_contract(const csdb::Transaction& tr) {
    if (is_smart_contract(tr)) {
        // must not be deploy/execute/new_state transaction
        return false;
    }
    return in_known_contracts(tr.target());
}

std::optional<api::SmartContractInvocation> SmartContracts::get_smart_contract_impl(const csdb::Transaction& tr) {
    // currently calls to is_***() from this method are prohibited, infinite recursion is possible!
    using namespace trx_uf;

    bool is_replenish_contract = false;
    if (!is_smart_contract(tr)) {
        is_replenish_contract = is_payable_target(tr);
        if (!is_replenish_contract) {
            return std::nullopt;
        }
    }

    const csdb::Address abs_addr = absolute_address(tr.target());

    // get info from private contracts table (faster), not from API

    if (is_new_state(tr) || is_replenish_contract) {
        auto maybe_contract = find_deploy_info(abs_addr);
        if (maybe_contract.has_value()) {
            return maybe_contract;
        }
    }
    // is executable (deploy or start):
    else {
        const csdb::UserField fld = tr.user_field(deploy::Code);  // start::Methods == deploy::Code, so does not matter what type of executable is
        if (fld.is_valid()) {
            std::string data = fld.value<std::string>();
            if (!data.empty()) {
                auto invoke = deserialize<api::SmartContractInvocation>(std::move(data));
                if (invoke.method.empty()) {
                    // is deploy
                    return std::make_optional(std::move(invoke));
                }
                else {
                    // is start
                    auto maybe_deploy = find_deploy_info(abs_addr);
                    if (maybe_deploy.has_value()) {
                        api::SmartContractInvocation& deploy = maybe_deploy.value();
                        deploy.method = invoke.method;
                        deploy.params = invoke.params;
                        return std::make_optional(deploy);
                    }
                }
            }
        }
    }

    return std::nullopt;
}

bool SmartContracts::is_payable_target(const csdb::Transaction& tr) {
    csdb::Address abs_addr = absolute_address(tr.target());
    if (!in_known_contracts(abs_addr)) {
        return false;
    }
    // may do blocking call to API::executor
    return is_payable(abs_addr);
}

void SmartContracts::enqueue(const csdb::Pool& block, size_t trx_idx) {
    if (trx_idx >= block.transactions_count()) {
        cserror() << kLogPrefix << "incorrect trx index in block to enqueue smart contract";
        return;
    }
    SmartContractRef new_item(block.hash().clone(), block.sequence(), trx_idx);
    csdb::Transaction t = block.transaction(trx_idx);
    csdb::Address abs_addr = absolute_address(t.target());

    auto it = find_in_queue(new_item);
    if (it != exe_queue.cend()) {
        csdebug() << kLogPrefix << "attempt to queue duplicated {" << new_item.sequence << '.' << new_item.transaction << "}, already queued on round #"
                    << it->seq_enqueue;
        return;
    }

    // test if this contract has already enqueued in this block
    for (it = exe_queue.begin(); it != exe_queue.end(); ++it) {
        if (it->seq_enqueue == new_item.sequence && it->abs_addr == abs_addr) {
            break;
        }
    }

    if (it == exe_queue.end()) {
        // enqueue to end
        [[maybe_unused]] bool payable = false;
        if (SmartContracts::is_deploy(t)) {
            // pre-register in known_contracts
            auto maybe_invoke_info = get_smart_contract_impl(t);
            if (maybe_invoke_info.has_value()) {
                const auto& invoke_info = maybe_invoke_info.value();
                StateItem& state = known_contracts[abs_addr];
                state.ref_deploy = new_item;
                if (update_metadata(invoke_info, state)) {
                    payable = implements_payable(state.payable);
                }
            }
        }
        else {
            // "lazy" metadata update, also covers cases of reading contracts from DB
            if (!is_metadata_actual(abs_addr)) {
                auto maybe_invoke_info = get_smart_contract_impl(t);
                if (maybe_invoke_info.has_value()) {
                    StateItem& state = known_contracts[abs_addr];
                    update_metadata(maybe_invoke_info.value(), state);
                }
            }
            payable = is_payable(abs_addr);
        }
        std::cout << std::endl; // emphasize with empty line
        cslog() << kLogPrefix << "enqueue " << print_executed_method(new_item);
        std::cout << std::endl; // emphasize with empty line
        it = exe_queue.emplace(exe_queue.cend(), QueueItem(new_item, abs_addr, t));
    }
    else {
        // add to existing queue item
        it->add(new_item, t);
        std::cout << std::endl; // emphasize with empty line
        cslog() << kLogPrefix << "add " << new_item << " to already enqueued contract";
        std::cout << std::endl; // emphasize with empty line
    }

    if (!it->executions.empty()) {
        execution_iterator execution = find_in_queue_item(it, new_item);
        if (execution == it->executions.end()) {
            // smth. strange, failed to find newly created item
            // nothing to do with it
            csdebug() << kLogPrefix << "logical error, unable to find just created execution item";
        }
        else {
            // in addition to contract "subcalls" set by transaction take more from contract's metadata
            const std::string method = get_executed_method_name(new_item);
            const size_t cnt_0 = execution->uses.size();
            add_uses_from(abs_addr, method, execution->uses);  // if failed, execution_allowed wil be set to false
            const size_t cnt = execution->uses.size();
            if (cnt > 0) {
                for (const auto& u : execution->uses) {
                    if (!in_known_contracts(u)) {
                        cslog() << kLogPrefix << "call to unknown contract declared in executing item, cancel {"
                            << new_item.sequence << '.' << new_item.transaction << '}';
                        remove_from_queue(new_item);
                        // also removes parent "it" from exe_queue if empty
                        return;
                    }
                }
            }
            if (cnt > cnt_0) {
                for (size_t i = cnt_0; i < cnt; ++i) {
                    execution->avail_fee -= execution->new_state_fee;  // reserve more fee for future new_state
                }
            }
            execution->consumed_fee += smart_round_fee(block);  // setup costs of initial round
        }
    }

    update_status(*it, new_item.sequence, SmartContractStatus::Waiting);
    it->is_executor = contains_me(block.confidants());
}

void SmartContracts::on_new_state(const csdb::Pool& block, size_t trx_idx) {
    if (!block.is_valid() || trx_idx >= block.transactions_count()) {
        cserror() << kLogPrefix << "incorrect new_state transaction specfied";
    }
    else {
        auto new_state = get_transaction(SmartContractRef{block.hash(), block.sequence(), trx_idx});
        if (!new_state.is_valid()) {
            cserror() << kLogPrefix << "get new_state transaction failed";
        }
        else {
            csdb::UserField fld_contract_ref = new_state.user_field(trx_uf::new_state::RefStart);
            if (!fld_contract_ref.is_valid()) {
                cserror() << kLogPrefix << "new_state transaction does not contain reference to contract";
            }
            else {
                SmartContractRef contract_ref(fld_contract_ref);
                // update state
                update_contract_state(new_state, false);
                const csdb::Address abs_addr = absolute_address(new_state.target());
                const cs::PublicKey& key = abs_addr.public_key();
                std::cout << std::endl; // emphasize with empty line
                cslog() << kLogPrefix << '{' << contract_ref.sequence << '.' << contract_ref.transaction << "} (" << EncodeBase58(key.data(), key.data() + key.size())
                        << ") state has been updated";
                std::cout << std::endl; // emphasize with empty line
                update_lock_status(abs_addr, false);
                remove_from_queue(contract_ref);
            }
            csdb::UserField fld_fee = new_state.user_field(trx_uf::new_state::Fee);
            if (fld_fee.is_valid()) {
                csdebug() << kLogPrefix << "contract execution fee " << fld_fee.value<csdb::Amount>().to_double();
                csdebug() << kLogPrefix << "contract new state fee " << new_state.counted_fee().to_double();
            }
        }
    }

    test_exe_queue();
}

void SmartContracts::test_exe_queue() {
    // update queue items status
    auto it = exe_queue.begin();
    while (it != exe_queue.end()) {
        if (it->status == SmartContractStatus::Closed) {
            csdebug() << kLogPrefix << "finished {" << it->seq_enqueue << ".*} still in queue, remove it";
            it = remove_from_queue(it);
            continue;
        }
        if (it->executions.empty()) {
            // the senseless item in the queue
            csdebug() << kLogPrefix << "empty {" << it->seq_enqueue << ".*} in queue, remove it";
            it = remove_from_queue(it);
            continue;
        }
        if (it->status == SmartContractStatus::Running) {
            // some contract is already running
            ++it;
            continue;
        }
        if (it->status == SmartContractStatus::Finished) {
            // some contract is under consensus
            ++it;
            continue;
        }
        // status: Waiting

        // is locked:
        bool wait_until_unlock = false;
        if (is_locked(it->abs_addr)) {
            csdebug() << kLogPrefix << '{' << it->seq_enqueue << ".*} still is locked, wait until unlocked";
            wait_until_unlock = true;
        }
        // is anyone of using locked:
        else {
            for (const auto& execution : it->executions) {
                for (const auto& u : execution.uses) {
                    if (is_locked(absolute_address(u))) {
                        csdebug() << kLogPrefix << "some contract using by {" << execution.ref_start.sequence << '.'
                            << execution.ref_start.transaction << "} still is locked, wait until unlocked";
                        wait_until_unlock = true;
                        break;
                    }
                }
            }
        }
        if (wait_until_unlock) {
            ++it;
            continue;
        }

        csdebug() << kLogPrefix << "set running status to {" << it->seq_enqueue << ".*} containing " << it->executions.size() << " jobs";
        update_status(*it, bc.getLastSequence(), SmartContractStatus::Running);
        // call to executor only if is trusted relatively to this contract
        if (it->is_executor || force_execution) {
            // final decision to execute contract is here, based on executor availability
            if (it->is_executor && !execution_allowed && !test_executor_availability()) {
                cslog() << kLogPrefix << "skip {" << it->seq_enqueue << ".*}, execution is not allowed (executor is not connected)";
                it->is_executor = false;
                // notify partners that unable to play trusted role
                bool fake_sent = false;
                const auto& confidants = pnode->retriveSmartConfidants(it->seq_enqueue);
                for (auto itconf = confidants.cbegin(); itconf != confidants.cend(); ++itconf) {
                    if (std::equal(itconf->cbegin(), itconf->cend(), node_id.cbegin())) {
                        cslog() << kLogPrefix << "unable to execute {" << it->seq_enqueue << ".*}, so send fake stage-1 & stage-2";
                        cs::Byte own_conf_num = cs::Byte(itconf - confidants.cbegin());
                        // empty it->executions tested above, so it is safe to call to front()
                        const auto& ref_start = it->executions.front().ref_start;
                        uint64_t id = SmartConsensus::createId(ref_start.sequence, uint16_t(ref_start.transaction), 0);
                        SmartConsensus::sendFakeStageOne(pnode, confidants, own_conf_num, id);
                        SmartConsensus::sendFakeStageTwo(pnode, confidants, own_conf_num, id);
                        fake_sent = true;
                        break;
                    }
                }
                if (!fake_sent) {
                    cslog() << kLogPrefix << "unable to execute {" << it->seq_enqueue << ".*} and failed to send fake stage-1 & stage-2";
                }
            }
            else {
                csdebug() << kLogPrefix << "execute {" << it->seq_enqueue << ".*} now";
                execute_async(it->executions);
            }
        }
        else {
            csdebug() << kLogPrefix << "skip {" << it->seq_enqueue << ".*} execution, not in trusted list";
        }

        ++it;
    }
}

SmartContractStatus SmartContracts::get_smart_contract_status(const csdb::Address& addr) const {
    if (!exe_queue.empty()) {
        const auto it = find_first_in_queue(absolute_address(addr));
        if (it != exe_queue.cend()) {
            return it->status;
        }
    }
    return SmartContractStatus::Idle;
}

bool SmartContracts::capture_transaction(const csdb::Transaction& tr) {
    cs::Lock lock(public_access_lock);

    // test smart contract as source of transaction
    // the new_state transaction is unable met here, we are the only one source of new_state
    csdb::Address abs_addr = absolute_address(tr.source());
    if (in_known_contracts(abs_addr)) {
        csdebug() << kLogPrefix << "smart contract is not allowed to emit transaction via API, drop it";
        return true;  // avoid from conveyer sync
    }

    // test smart contract as target of transaction (is it payable?)
    abs_addr = absolute_address(tr.target());
    bool is_contract = false;
    bool has_state = false;
    const auto it = known_contracts.find(abs_addr);
    if (it != known_contracts.end()) {
        is_contract = true;
        has_state = !it->second.state.empty();
    }

    if (is_contract) {
        // test contract was deployed (and maybe called successfully)
        if (!has_state) {
            cslog() << kLogPrefix << "unable execute not successfully deployed contract, drop transaction";
            return true;  // block from conveyer sync
        }

        double amount = tr.amount().to_double();
        // possible blocking call to executor for the first time:
        if (!is_payable(abs_addr)) {
            if (amount > std::numeric_limits<double>::epsilon()) {
                cslog() << kLogPrefix << "unable replenish balance of contract without payable() feature, drop transaction";
                return true;  // block from conveyer sync
            }
            else /*amount is 0*/ {
                if (!is_smart_contract(tr)) {
                    // not deploy/execute/new_state transaction as well as smart is not payable
                    cslog() << kLogPrefix << "unable call to payable(), feature is not implemented in contract, drop transaction";
                    return true;  // block from conveyer sync
                }
            }
        }
        else /* is payable */ {
            // test if payable() is not directly called
            if (is_executable(tr)) {
                const csdb::UserField fld = tr.user_field(cs::trx_uf::start::Methods);
                if (fld.is_valid()) {
                    std::string data = fld.value<std::string>();
                    if (!data.empty()) {
                        auto invoke = deserialize<api::SmartContractInvocation>(std::move(data));
                        if (invoke.method == PayableName) {
                            cslog() << kLogPrefix << "unable call to payable() directly, drop transaction";
                            return true;  // block from conveyer sync
                        }
                    }
                }
                csdebug() << kLogPrefix << "allow deploy/executable transaction";
            }
            else /* not executable transaction */ {
                // contract is payable and transaction addresses it, ok then
                csdebug() << kLogPrefix << "allow transaction to target payable contract";
            }
        }
    }

    if (SmartContracts::is_deploy(tr)) {
        csdebug() << kLogPrefix << "deploy transaction detected";
    }
    else if (SmartContracts::is_start(tr)) {
        csdebug() << kLogPrefix << "start transaction detected";
    }

    return false;  // allow pass to conveyer sync
}

bool SmartContracts::test_executor_availability() {
    if (!execution_allowed) {
        execution_allowed = exec_handler_ptr && exec_handler_ptr->getExecutor().isConnect();
        if (execution_allowed) {
            cslog() << std::endl << kLogPrefix << "connection to executor is restored" << std::endl;
            // update all currently running contracts locks, missed while executor was unavailable
            for (const auto& exe_item : exe_queue) {
                if (exe_item.status == SmartContractStatus::Running || exe_item.status == SmartContractStatus::Finished) {
                    if (!is_metadata_actual(exe_item.abs_addr)) {
                        auto maybe_deploy = find_deploy_info(exe_item.abs_addr);
                        if (maybe_deploy.has_value()) {
                            auto it_state = known_contracts.find(exe_item.abs_addr);
                            if (it_state != known_contracts.end()) {
                                if (!update_metadata(maybe_deploy.value(), it_state->second)) {
                                    if (!execution_allowed) {
                                        // the problem has got back
                                        break;
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    return execution_allowed;
}

CallsQueueScheduler& SmartContracts::getScheduler() {
    return scheduler;
}

void SmartContracts::on_store_block(const csdb::Pool& block) {
    cs::Lock lock(public_access_lock);

    test_executor_availability();
    test_exe_conditions(block);
    test_exe_queue();
    test_contracts_locks();

    // inspect transactions against smart contracts, raise special event on every item found:
    if (block.transactions_count() > 0) {
        size_t tr_idx = 0;
        for (const auto& tr : block.transactions()) {
            if (is_smart_contract(tr)) {
                // dispatch transaction by its type
                bool is_deploy = this->is_deploy(tr);
                bool is_start = is_deploy ? false : this->is_start(tr);
                if (is_deploy || is_start) {
                    if (is_deploy) {
                        csdebug() << kLogPrefix << "contract is deployed by #" << block.sequence() << "." << tr_idx;
                    }
                    else {
                        csdebug() << kLogPrefix << "contract is invoked by #" << block.sequence() << "." << tr_idx;
                    }
                    enqueue(block, tr_idx);
                }
                else if (is_new_state(tr)) {
                    csdebug() << kLogPrefix << "contract state is updated by #" << block.sequence() << "." << tr_idx;
                    on_new_state(block, tr_idx);
                }
            }
            else if (is_payable_target(tr)) {
                // execute payable method
                csdebug() << kLogPrefix << "contract balance is replenished by #" << block.sequence() << "." << tr_idx;
                emit signal_payable_invoke(tr);
                enqueue(block, tr_idx);
            }
            else {
                // test is emitted by contract
                csdb::Address abs_addr = absolute_address(tr.source());
                const auto it = known_contracts.find(abs_addr);
                if (it != known_contracts.cend()) {
                    // is emitted by contract
                    const auto& state = it->second;
                    csdb::Transaction starter = get_transaction(state.ref_execute);
                    if (implements_payable(state.payable) && starter.is_valid()) {
                        emit signal_emitted_accepted(tr, starter);
                    }
                    else {
                        cserror() << kLogPrefix << "failed to find starter transaction for contract emitted one";
                    }
                }
            }
            ++tr_idx;
        }
    }

    test_exe_queue();
}

void SmartContracts::on_read_block(const csdb::Pool& block, bool* /*should_stop*/) {
    cs::Lock lock(public_access_lock);

    // uncomment when exe_queue is updated during blocks reading on startup:
    // test_exe_conditions(block);

    // control round-based timeout
    // assume block arrive in increasing sequence order
    while (!replenish_contract.empty()) {
        const auto it = replenish_contract.cbegin();
        if (block.sequence() - it->sequence <= Consensus::MaxRoundsCancelContract) {
            // no timeout yet
            break;
        }
        csdb::Transaction t = get_transaction(*it);
        if (t.is_valid()) {
            emit signal_payable_timeout(t);
        }
        replenish_contract.erase(it);
    }

    if (block.transactions_count() > 0) {
        size_t tr_idx = 0;
        for (const auto& tr : block.transactions()) {
            if (is_new_state(tr)) {
                update_contract_state(tr, true);
            }
            else {
                csdb::Address abs_addr = absolute_address(tr.target());
                if (!abs_addr.is_valid()) {
                    cserror() << kLogPrefix << "failed convert optimized address";
                }
                else {
                    if (!in_known_contracts(abs_addr)) {
                        if (is_deploy(tr)) {
                            // register ONLY contract deploy,
                            // known contracts will be updated on new_state handling
                            StateItem& state = known_contracts[abs_addr];
                            state.ref_deploy.hash = block.hash();
                            state.ref_deploy.sequence = block.sequence();
                            state.ref_deploy.transaction = tr_idx;
                        }
                    }
                    else {
                        if (!is_executable(tr)) {
                            // replenish smart contract
                            emit signal_payable_invoke(tr);
                            replenish_contract.emplace_back(block.hash(), block.sequence(), tr_idx);
                        }
                    }
                }
            }

            ++tr_idx;
        }
    }

    // do not assign stop flag not to overwrite value set by other subscribers:
    //*should_stop = false;
}

// tests max fee amount and round-based timeout on executed smart contracts;
// is invoked on every new block
void SmartContracts::test_exe_conditions(const csdb::Pool& block) {
    if (exe_queue.empty()) {
        return;
    }

    const auto seq = block.sequence();
    for (auto& item : exe_queue) {
        if (item.status != SmartContractStatus::Running && item.status != SmartContractStatus::Finished) {
            continue;
        }

        // smart is in executor or is under smart-consensus

        // unconditional timeout, actual for both Finished and Running items
        if (seq > item.seq_start && seq - item.seq_start > Consensus::MaxRoundsCancelContract) {
            cswarning() << kLogPrefix << '{' << item.seq_enqueue << ".*} is in queue over " << Consensus::MaxRoundsCancelContract
                        << " blocks (from #" << item.seq_start << "), remove it without transaction";
            update_status(item, seq, SmartContractStatus::Closed);
            for (const auto& execution : item.executions) {
                csdb::Transaction starter = get_transaction(execution.ref_start);
                if (starter.is_valid()) {
                    if (!is_executable(starter)) {
                        emit signal_payable_timeout(starter);
                    }
                }
                else {
                    cserror() << kLogPrefix << "cannot handle {" << execution.ref_start.sequence << '.' << execution.ref_start.transaction
                        << "} execution timeout properly, starter transaction not found";
                }
            }
            continue;
        }

        if (item.status == SmartContractStatus::Running) {
            // test near-timeout:
            if (seq > item.seq_start && seq - item.seq_start > Consensus::MaxRoundsExecuteContract) {
                cslog() << kLogPrefix << '{' << item.seq_enqueue << ".*} is in queue over " << Consensus::MaxRoundsExecuteContract
                        << " blocks (from #" << item.seq_start << "), stop it";
                if (item.is_executor) {
                    std::vector<SmartExecutionData> data_list;
                    for (const auto& execution : item.executions) {
                        SmartExecutionData& data = data_list.emplace_back();
                        data.contract_ref = execution.ref_start;
                        data.setError(error::TimeExpired, "contract execution timeout");
                    }
                    if (!data_list.empty()) {
                        on_execution_completed_impl(std::move(data_list));
                    }
                }
                else {
                    update_status(item, seq, SmartContractStatus::Finished);
                }
                continue;
            }
            // test out-of-fee in every execution item and cancel all jobs if any out-of-fee occurs
            const auto add_fee = smart_round_fee(block);
            for (auto& execution : item.executions) {
                execution.consumed_fee += add_fee;
                if (execution.avail_fee < execution.consumed_fee) {
                    // cancel all item and break the loop
                    cslog() << kLogPrefix << '{' << execution.ref_start.sequence << '.' << execution.ref_start.transaction
                        << "} is out of fee, cancel the whole queue item";
                    if (item.is_executor) {
                        std::vector<SmartExecutionData> data_list;
                        for (const auto& e: item.executions) {
                            SmartExecutionData& data = data_list.emplace_back();
                            data.contract_ref = e.ref_start;
                            data.setError(error::OutOfFunds, "contract execution is out of funds");
                        }
                        if (!data_list.empty()) {
                            on_execution_completed_impl(std::move(data_list));
                        }
                    }
                    else {
                        update_status(item, seq, SmartContractStatus::Finished);
                    }
                    break;
                }
            }
        }  // if block for Running only contract

    }  // for each exe_queue item
}

// return next element in queue
SmartContracts::queue_iterator SmartContracts::remove_from_queue(SmartContracts::queue_iterator it) {
    if (it != exe_queue.cend()) {
        cslog() << kLogPrefix << "remove from queue completed item {" << it->seq_enqueue << ".*}";
        for (const auto item : it->executions) {
            cslog() << "\t{" << item.ref_start.sequence << '.' << item.ref_start.transaction << "} " << print_executed_method(item.ref_start);
        }
        const cs::Sequence seq = bc.getLastSequence();
        const cs::Sequence seq_cancel = it->seq_start + Consensus::MaxRoundsCancelContract + 1;
        if (seq > it->seq_start + Consensus::MaxRoundsExecuteContract && seq < seq_cancel) {
            cslog() << kLogPrefix << seq_cancel - seq << " round(s) remains until unconditional timeout";
        }
        // its too early to unlock contract(s), wait until states will updated
        // unlock only closed (after timeout) contracts
        if (it->status == SmartContractStatus::Closed) {
            update_lock_status(*it, false);
        }
        it = exe_queue.erase(it);

        if (exe_queue.empty()) {
            csdebug() << kLogPrefix << "contract queue is empty, nothing to execute";
        }
        else {
            csdebug() << kLogPrefix << exe_queue.size() << " item(s) in queue";
        }
    }

    return it;
}

void SmartContracts::remove_from_queue(const SmartContractRef& item) {
    queue_iterator it = find_in_queue(item);
    if (it == exe_queue.end()) {
        return;
    }
    auto execution = find_in_queue_item(it, item);
    if (execution != it->executions.cend()) {
        cslog() << kLogPrefix << "remove from queue completed {"
            << execution->ref_start.sequence << '.' << execution->ref_start.transaction << "} " << print_executed_method(execution->ref_start);
        it->executions.erase(execution);
    }
    if (it->executions.empty()) {
        remove_from_queue(it);
    }
}

bool SmartContracts::execute(SmartExecutionData& data) {
    if (!data.result.smartsRes.empty()) {
        data.result.smartsRes.clear();
    }
    if (!exec_handler_ptr) {
        data.setError(error::ExecuteTransaction, "contract executor is unavailable");
        return false;
    }
    csdb::Transaction transaction = get_transaction(data.contract_ref);
    if (!transaction.is_valid()) {
        data.setError(error::InternalBug, "load starter transaction failed");
        return false;
    }
    cslog() << kLogPrefix << "executing " << data.contract_ref << "::" << print_executed_method(data.contract_ref) << std::endl;
    // using data.result.newState to pass previous (not yet cached) new state in case of multi-call to conrtract:
    std::vector<executor::Executor::ExecuteTransactionInfo> smarts;
    auto& info = smarts.emplace_back(executor::Executor::ExecuteTransactionInfo{});
    info.transaction = transaction;
    
    info.feeLimit = data.executor_fee;
    info.convention = executor::Executor::MethodNameConvention::Default;
    if (!is_smart(transaction)) {
        // the most frequent fast test
        auto item = known_contracts.find(absolute_address(transaction.target()));
        if (item != known_contracts.end()) {
            StateItem& state = item->second;
            if (state.payable == PayableStatus::Implemented) {
                info.convention = executor::Executor::MethodNameConvention::PayableLegacy;
            }
            else if (state.payable == PayableStatus::ImplementedVer1) {
                info.convention = executor::Executor::MethodNameConvention::Payable;
            }
        }
    }
    auto maybe_result = exec_handler_ptr->getExecutor().executeTransaction(smarts, data.explicit_last_state);
    if (maybe_result.has_value()) {
        data.result = maybe_result.value();
        if (!data.result.smartsRes.empty()) {
            if (data.result.response.code == 0) {
                auto& result = data.result.smartsRes.front();
                if (result.response.code == 0) {
                    // calculate execution fee
                    csdb::Amount total_fee(0);
                    for (const auto r : data.result.smartsRes) {
                        total_fee += fee::getExecutionFee(r.executionCost);
                    }
#if defined(USE_SELF_MEASURED_FEE)
                    if (total_fee.to_double() < DBL_EPSILON) {
                        total_fee = fee::getExecutionFee(data.result.selfMeasuredCost);
                    }
#endif
                    if (total_fee > data.executor_fee) {
                        // out of fee detected
                        data.setError(error::OutOfFunds, "contract execution is out of funds");
                    }
                    else {
                        // update with actual value
                        data.executor_fee = total_fee;
                    }
                } else {
                    data.error = result.response.message;
                    if (data.error.empty()) {
                        data.error = "contract execution failed, new contract state is empty";
                    }
                }
            }
            else {
                data.error = data.result.response.message;
                if (data.error.empty()) {
                    data.setError(error::ExecuteTransaction, "contract execution failed, contract state is unchanged");
                }
            }
        }
        else {
            // smart result is empty!
            data.setError(error::ExecuteTransaction, "contract execution failed, contract state is unchanged");
        }
    }
    else {
        data.setError(error::ExecuteTransaction, "contract execution failed");
    }
    return true;
}

// returns false if execution canceled, so caller may call to remove_from_queue()
bool SmartContracts::execute_async(const std::vector<ExecutionItem>& executions) {
    std::vector<SmartExecutionData> data_list;
    for (const auto& execution : executions) {
        SmartExecutionData& execution_data = data_list.emplace_back();
        execution_data.contract_ref = execution.ref_start;
        execution_data.executor_fee = execution.avail_fee;
        csdb::Transaction start_tr = get_transaction(execution.ref_start);
        bool replenish_only = false;  // means indirect call to payable()
        if (!is_executable(start_tr)) {
            replenish_only = is_payable_target(start_tr);
            if (!replenish_only) {
                // it must be filtered before not to prevent other calls from execution
                cserror() << kLogPrefix << "unable execute neither deploy nor start/replenish transaction";
                return false;
            }
        }
        bool deploy = is_deploy(start_tr);
        csdebug() << kLogPrefix << "invoke api to remote executor to " << (deploy ? "deploy" : (!replenish_only ? "execute" : "replenish"))
            << " {" << execution.ref_start.sequence << '.' << execution.ref_start.transaction << '}';
    }

    if (data_list.empty()) {
        // in fact, it was tested before
        return false;
    }

    // create runnable object
    auto runnable = [this, data_list{std::move(data_list)}]() mutable {
        if (!data_list.empty()) {

            // actually, multi-execution list always refers to the same contract, so we need not to distinct different contracts last state
            std::string last_state;
            for (auto& data : data_list) {
                // use data.result.newStatef member to pass last contract's state in multi-call
                data.explicit_last_state = last_state;
                if (!execute(data)) {
                    if (data.error.empty()) {
                        data.error = "failed to invoke contract";
                    }
                    // last_state is not updated
                }
                else {
                    // execute never returns empty data.result.smartsRes list
                    if (!data.result.smartsRes.empty()) {
                        // remember last state for the next execution
                        last_state = data.result.smartsRes.front().newState;
                    }
                }
            }
        }
        return data_list;
    };

    // run async and watch result
    auto watcher = cs::Concurrent::run(cs::RunPolicy::CallQueuePolicy, runnable);
    cs::Connector::connect(&watcher->finished, this, &SmartContracts::on_execution_completed);

    return true;
}

void SmartContracts::on_execution_completed_impl(const std::vector<SmartExecutionData>& data_list) {
    using namespace trx_uf;
    if (data_list.empty()) {
        // actually is checked before
        return;
    }

    // any of data item "points" to the same queue item
    auto it = find_in_queue(data_list.front().contract_ref);
    if (it != exe_queue.end()) {
        if (it->status == SmartContractStatus::Finished || it->status == SmartContractStatus::Closed) {
            // already finished (by timeout), no transaction required
            return;
        }
        update_status(*it, bc.getLastSequence(), SmartContractStatus::Finished);
    }
    else {
        return;
    }

    // create (multi-)packet:
    // new_state[0] + [ emitted_list[0] ] + [ susequent_state_list[0] ] + ... + new_state[n-1] + [ emitted_list[n-1] ] + [ subsequent_state_list[n-1] ]
    cs::TransactionsPacket integral_packet;
    int64_t next_id = 0; // "lazy" initialization assumed

    for (const auto& data_item : data_list) {
        ExecutionItem* execution = nullptr;
        // create partial new_state transaction
        if (it != exe_queue.end()) {
            auto it_exe = find_in_queue_item(it, data_item.contract_ref);
            csdebug() << kLogPrefix << "execution of " << data_item.contract_ref << " has completed";
            if (it_exe != it->executions.end()) {
                execution = &(*it_exe);
            }
        }
        if (execution == nullptr) {
            // wtf data without execution item?
            continue;
        }

        execution->consumed_fee = data_item.executor_fee;
        cs::TransactionsPacket& packet = execution->result;
        if (packet.transactionsCount() > 0) {
            packet.clear();
        }

        if (next_id > 0) {
            ++next_id;
        }
        else {
            // 1st-time init
            auto starter = get_transaction(execution->ref_start);
            if (starter.is_valid()) {
                next_id = next_inner_id(absolute_address(starter.target()));
            }
            else {
                next_id = 1;
            }
        }
        csdb::Transaction result = create_new_state(*execution, next_id);
        csdebug() << kLogPrefix << "set innerID = " << next_id << " in " << data_item.contract_ref << " new_state";

        // create partial failure if new_state is not created
        if(!result.is_valid()) {
            cserror() << kLogPrefix << "cannot find in queue just completed contract, so cannot create new_state";
            csdb::Transaction tmp = get_transaction(data_item.contract_ref);
            if (!tmp.is_valid()) {
                return;
            }
            QueueItem fake(data_item.contract_ref, absolute_address(tmp.target()), tmp);
            if (!fake.executions.empty()) {
                result = create_new_state(fake.executions.front(), next_id); // use the same next_id again
            }
            else {  
                // wtf!
                cserror() << kLogPrefix << "failed to create new_state transaction, even empty";
            }
        }

        // finalize new_state transaction, data_item.result.smartsRes always non-empty
        if (!data_item.error.empty()) {
            cserror() << std::endl << kLogPrefix << data_item.error << std::endl;
            csdebug() << kLogPrefix << "execution of " << data_item.contract_ref << " is failed, new state is empty";
            // result contains empty USRFLD[state::Value]
            result.add_user_field(new_state::Value, std::string{});
            // result contains error code from ret_val
            set_return_value(result, data_item.result.smartsRes.front().retValue);
            packet.addTransaction(result);
        }
        else {
            const auto& execution_result = data_item.result.smartsRes.front();
            csdebug() << kLogPrefix << "execution of " << data_item.contract_ref << " is successful, new state size = " << execution_result.newState.size();

            // put new state
            result.add_user_field(new_state::Value, execution_result.newState);
            set_return_value(result, execution_result.retValue);
            packet.addTransaction(result);

            if (it != exe_queue.end()) {
                // put emitted transactions
                if (!data_item.result.trxns.empty()) {
                    for (const auto& tr : data_item.result.trxns) {
                        if (tr.innerID() == 0) {
                            // auto inner id generating
                            csdb::Transaction tmp = tr.clone();
                            tmp.set_innerID(++next_id);
                            csdebug() << kLogPrefix << "set innerID = " << next_id << " in " << data_item.contract_ref << " emitted transaction";
                            packet.addTransaction(tmp);
                        }
                        else {
                            packet.addTransaction(tr);
                        }
                    }
                    csdebug() << kLogPrefix << "add " << data_item.result.trxns.size() << " emitted transaction(s) to " << data_item.contract_ref << " state";
                }
                else {
                    csdebug() << kLogPrefix << "no emitted transaction added to " << data_item.contract_ref;
                }
                // put subsequent new_states if any
                if (!data_item.result.states.empty()) {
                    csdebug() << kLogPrefix << "add " << data_item.result.states.size() << " subsequent new state(s) along with " << data_item.contract_ref << " state";
                    for (const auto& [addr, state] : data_item.result.states) {
                        auto it_call = find_in_queue_item(it, data_item.contract_ref);
                        if (it_call != it->executions.end()) {
                            csdb::Transaction t = create_new_state(*it_call, ++next_id);
                            csdebug() << kLogPrefix << "set innerID = " << next_id << " in " << data_item.contract_ref << " secondary contract new_state";
                            if (t.is_valid()) {
                                // re-assign some fields
                                t.set_innerID(next_inner_id(addr));
                                t.set_source(addr);
                                t.set_target(addr);
                                t.add_user_field(trx_uf::new_state::Value, state);
                                t.add_user_field(trx_uf::new_state::Fee, csdb::Amount(0));
                                set_return_value(t, ::general::Variant{});
                                packet.addTransaction(t);
                            }
                        }
                    }
                }
            }
        }
        // add all transactions to integral packet
        for (const auto& t : packet.transactions()) {
            integral_packet.addTransaction(t);
        }
    }

    // 'it' already has tested
    std::ostringstream os;
    for (const auto e : it->executions) {
        os << e.ref_start << ' ';
    }
    csdebug() << kLogPrefix << "starting " << os.str() << "consensus";
    if (!it->is_executor || !start_consensus(*it, integral_packet)) {
        cserror() << kLogPrefix << os.str() << "consensus is not started, remove item from queue";
        remove_from_queue(it);
    }

    // inform slots if any, packet does not contain smart consensus' data!
    emit signal_smart_executed(integral_packet);
}

uint64_t SmartContracts::next_inner_id(const csdb::Address& addr) const {
    csdb::Address abs_addr = SmartContracts::absolute_address(addr);
    
    // lookup in blockchain
    BlockChain::WalletData wallData{};
    BlockChain::WalletId wallId{};
    uint64_t id = 1;
    if (bc.findWalletData(abs_addr, wallData, wallId)) {
        if (!wallData.trxTail_.empty()) {
            id = wallData.trxTail_.getLastTransactionId() + 1;
        }
    }
    //csdebug() << kLogPrefix << "next innerID " << id << " (from storage)";
    return id;
}

csdb::Transaction SmartContracts::create_new_state(const ExecutionItem& item, int64_t new_id) {
    csdb::Transaction src = get_transaction(item.ref_start);
    if (!src.is_valid()) {
        return csdb::Transaction{};
    }
    csdb::Transaction result(new_id,        
                             src.target(),      // contract's address
                             src.target(),      // contract's address
                             src.currency(),    // source value
                             0,                 // amount
                             csdb::AmountCommission((item.avail_fee - item.consumed_fee).to_double()), csdb::AmountCommission(item.new_state_fee.to_double()),
                             Zero::signature  // empty signature
    );
    // USRFLD1 - ref to start trx
    result.add_user_field(trx_uf::new_state::RefStart, item.ref_start.to_user_field());
    // USRFLD2 - total fee
    result.add_user_field(trx_uf::new_state::Fee, item.consumed_fee);
    return result;
}

// get & handle rejected transactions
// the aim is
//  - to store completed executions
//  - repeat consensus for rejected executions fixing empty new_states
//  - re-execute valid but "compromised" by rejected items executions
void SmartContracts::on_reject(const std::vector<Node::RefExecution>& reject_list) {

    if (reject_list.empty()) {
        return;
    }

    cs::RoundNumber current_sequence = bc.getLastSequence();

    cs::Lock lock(public_access_lock);

    // handle failed calls
    csdebug() << kLogPrefix << "get reject contract(s) signal";
    if (reject_list.empty()) {
        csdebug() << kLogPrefix << "rejected contract list is empty";
    }
    else {
        csdebug() << kLogPrefix << "" << reject_list.size() << " contract(s) are rejected";

        // group reject_list by block sequence
        std::map< cs::Sequence, std::list<uint16_t> > grouped_failed;
        for (const auto& item : reject_list) {
            grouped_failed[item.first].emplace_back(item.second);
        }

        for (const auto& [sequence, executions] : grouped_failed) {
            if (executions.empty()) {
                // actually impossible
                continue;
            }
            // to store newly created items:
            decltype(exe_queue) new_queue_items;
            auto it_queue = exe_queue.begin();
            while (it_queue != exe_queue.end()) {
                if (it_queue->seq_enqueue == sequence) {
                    auto it_exe = it_queue->executions.begin();
                    while (it_exe != it_queue->executions.end()) {
                        if (std::find(executions.cbegin(), executions.cend(), it_exe->ref_start.transaction) != executions.cend()) {
                            // found (maybe partially) rejected queue item
                            if (it_queue->is_rejected) {
                                // has alredy done before
                                break;
                            }
                            // it_exe here points to the first rejected call in multi-call
                            // replace all rejected items with empty new state
                            std::vector<ExecutionItem> reject;
                            reject.emplace_back(*it_exe);
                            it_exe = it_queue->executions.erase(it_exe);
                            // schedule re-execution of subsequent non-rejected items
                            std::vector<ExecutionItem> restart;

                            // starting inner sub-cycle
                            while (it_exe != it_queue->executions.end()) {
                                if (std::find(executions.cbegin(), executions.cend(), it_exe->ref_start.transaction) != executions.cend()) {
                                    // do not clear result, new_state is required
                                    reject.emplace_back(*it_exe);
                                }
                                else {
                                    // empty result pack required
                                    it_exe->result.clear();
                                    restart.emplace_back(*it_exe);
                                }
                                it_exe = it_queue->executions.erase(it_exe);
                            }

                            // it_exe now is equal to it_queue->executions.end(), do not use it!!!

                            // finally create 1 otr 2 new queue items
                            if (!reject.empty()) {
                                QueueItem& new_rejected_item = new_queue_items.emplace_back(it_queue->fork());
                                new_rejected_item.executions.assign(reject.cbegin(), reject.cend());
                                update_status(new_rejected_item, current_sequence, SmartContractStatus::Finished);
                                new_rejected_item.is_rejected = true;
                                cs::TransactionsPacket integral_pack;
                                for (auto& e : new_rejected_item.executions) {
                                    for (auto& t : e.result.transactions()) {
                                        // lookup proper new state, erase other transactions in result
                                        if (SmartContracts::is_new_state(t) && SmartContracts::absolute_address(t.target()) == new_rejected_item.abs_addr) {
                                            t.add_user_field(trx_uf::new_state::Value, std::string{});
                                            set_return_value(t, error::ConsensusRejected);
                                            integral_pack.addTransaction(t);
                                            e.result.clear();
                                            e.result.addTransaction(t);
                                            break;
                                        }
                                    }
                                }
                                start_consensus(new_rejected_item, integral_pack);
                            }
                            if (!restart.empty()) {
                                QueueItem& new_restart_item = new_queue_items.emplace_back(it_queue->fork());
                                new_restart_item.executions.assign(restart.cbegin(), restart.cend());
                                update_status(new_restart_item, current_sequence, SmartContractStatus::Waiting);
                            }
                            csdebug() << kLogPrefix << "{" << sequence << "*.} is splitted onto " << it_queue->executions.size() << " completed + "
                                << reject.size() << " rejected + " << restart.size() << " restart calls";
                            break;
                        }
                        if (it_exe == it_queue->executions.end()) {
                            break;
                        }
                        ++it_exe;
                    }
                }
                if (it_queue->executions.empty()) {
                    // all jobs are rejected/restarted
                    it_queue = exe_queue.erase(it_queue);
                }
                if (it_queue == exe_queue.end()) {
                    break;
                }
                ++it_queue;
            }
            // add new items if any
            if (!new_queue_items.empty()) {
                exe_queue.insert(exe_queue.end(), new_queue_items.cbegin(), new_queue_items.cend());
            }
        }
    }

    test_exe_queue();
}

bool SmartContracts::update_contract_state(const csdb::Transaction& t, bool reading_db) {
    using namespace trx_uf;
    csdb::UserField fld = t.user_field(new_state::Value);
    if (!fld.is_valid()) {
        cserror() << kLogPrefix << "contract state is not updated, transaction does not contain it";
        return false;
    }
    std::string state_value = fld.value<std::string>();
    if (!state_value.empty()) {
        if (!reading_db) {
            std::cout << std::endl; // emphasize with empty line
            if (state_value.empty()) {
                cslog() << kLogPrefix << "last contract state remains unchanged, execution failed";
            }
            else {
                cslog() << kLogPrefix << "contract state is updated, new size is " << state_value.size();
            }
            std::cout << std::endl; // emphasize with empty line
        }
        // create or get contract state item
        csdb::Address abs_addr = absolute_address(t.target());
        if (abs_addr.is_valid()) {
            StateItem& item = known_contracts[abs_addr];
            item.state = std::move(state_value);
            // determine it is the result of whether deploy or execute
            fld = t.user_field(new_state::RefStart);
            if (fld.is_valid()) {
                SmartContractRef ref(fld);
                csdb::Transaction t_start = get_transaction(ref);
                if (t_start.is_valid()) {
                    if (is_executable(t_start)) {
                        if (is_deploy(t_start)) {
                            item.ref_deploy = ref;
                        }
                        else {
                            item.ref_execute = ref;
                        }
                    }
                    else {
                        // new_state after replenish contract transaction
                        if (reading_db) {
                            // handle replenish during startup reading
                            const auto it = std::find(replenish_contract.cbegin(), replenish_contract.cend(), ref);
                            if (it != replenish_contract.cend()) {
                                replenish_contract.erase(it);
                            }
                            else {
                                csdebug() << kLogPrefix << "(error in blockchain) cannot find replenish transaction new_state refers to";
                            }
                        }
                        else {
                            // handle replenish from on-the-air blocks
                            if (!implements_payable(item.payable)) {
                                cserror() << kLogPrefix << "non-payable contract state is updated by replenish transaction";
                            }
                        }
                        item.ref_execute = ref;
                    }
                }
                else {
                    if (reading_db) {
                        csdebug() << kLogPrefix << "(error in blockchain) cannot find starter transaction new_state refer to";
                    }
                    else {
                        cswarning() << kLogPrefix << "new_state transaction does not refer to starter one";
                    }
                }
            }
        }
        else {
            if (reading_db) {
                csdebug() << kLogPrefix << "(error in blockchain) cannot find contract by address from new_state";
            }
            else {
                cserror() << kLogPrefix << "failed to convert optimized address";
            }
        }
    }
    else {
        // state_value is empty - erase replenish_contract item if exists
        if (!replenish_contract.empty()) {
            fld = t.user_field(new_state::RefStart);
            if (fld.is_valid()) {
                SmartContractRef ref(fld);
                csdb::Transaction t_start = get_transaction(ref);
                if (t_start.is_valid()) {
                    // handle replenish during startup reading
                    const auto it = std::find(replenish_contract.cbegin(), replenish_contract.cend(), ref);
                    if (it != replenish_contract.cend()) {
                        replenish_contract.erase(it);
                    }
                }
            }
        }
        std::string error_message("execution is failed");
        fld = t.user_field(new_state::RetVal);
        if (fld.is_valid()) {
            ::general::Variant var = deserialize <::general::Variant>(fld.value<std::string>());
            if (var.__isset.v_byte) {
                error_message = SmartContracts::get_error_message(var.v_byte);
            }
            else if (var.__isset.v_string) {
                error_message = var.v_string;
            }
        }
        csdebug() << kLogPrefix << "contract state is not updated, " << error_message;
        return false;
    }
    return true;
}

bool SmartContracts::is_payable(const csdb::Address& abs_addr) {
    // the most frequent fast test
    auto item = known_contracts.find(abs_addr);
    if (item == known_contracts.end()) {
        // unknown contract
        return false;
    }

    StateItem& state = item->second;
    if (state.payable != PayableStatus::Unknown) {
        return implements_payable(state.payable);
    }

    // the first time test
    auto maybe_deploy = find_deploy_info(abs_addr);
    if (!maybe_deploy.has_value()) {
        // smth goes wrong, do not update contract state but return false result
        return false;
    }
    if (!update_metadata(maybe_deploy.value(), state)) {
        return false;
    }
    return implements_payable(state.payable);
}

bool SmartContracts::update_metadata(const api::SmartContractInvocation& contract, StateItem& state) {
    if (!exec_handler_ptr) {
        return false;
    }
    executor::GetContractMethodsResult result;
    std::string error;
    auto& executor_instance = exec_handler_ptr->getExecutor();
    executor_instance.getContractMethods(result, contract.smartContractDeploy.byteCodeObjects);
    if (result.status.code != 0) {
        execution_allowed = executor_instance.isConnect();
        if (!result.status.message.empty()) {
            cswarning() << kLogPrefix << result.status.message;
        }
        else {
            if (!execution_allowed) {
                cswarning() << kLogPrefix << "unable to connect to executor";
            }
            else {
                cswarning() << kLogPrefix << "execution error " << int(result.status.code);
            }
        }
        // remain payable status & using unknown for future calls
        return false;
    }

    state.payable = PayableStatus::Absent;
    // lookup payable(amount, currency) && annotations
    for (const auto& m : result.methods) {
        // payable status, continue tests if PayableStatus::Implemented, not PayableStatus::ImplementedVer1
        if (state.payable != PayableStatus::ImplementedVer1) {
            if (m.name == PayableName) {
                if (m.arguments.size() == 2) {
                    const auto& a0 = m.arguments[0];
                    if (m.returnType == TypeVoid && a0.type == TypeString) {
                        const auto& a1 = m.arguments[1];
                        if (a1.type == TypeString) {
                            state.payable = PayableStatus::Implemented;
                        }
                    }
                    else if (m.returnType == TypeString && a0.type == TypeBigDecimal) {
                        const auto& a1 = m.arguments[1];
                        if (a1.type == TypeByteArray) {
                            state.payable = PayableStatus::ImplementedVer1;
                        }
                    }
                }
            }
        }
        // uses
        if (!m.annotations.empty()) {
            for (const auto& a : m.annotations) {
                if (a.name == UsesContract) {
                    csdb::Address addr;
                    std::string method;
                    if (a.arguments.count(UsesContractAddr) > 0) {
                        std::vector<uint8_t> bytes;
                        if (DecodeBase58(a.arguments.at(UsesContractAddr), bytes)) {
                            addr = csdb::Address::from_public_key(bytes);
                            if (addr.is_valid()) {
                                if (a.arguments.count(UsesContractMethod) > 0) {
                                    method = a.arguments.at(UsesContractMethod);
                                }
                                auto& u = state.uses[m.name];
                                u[addr] = method;  // empty method name is allowed too
                            }
                        }
                    }
                }
            }
        }
    }

    return true;
}

void SmartContracts::add_uses_from(const csdb::Address& abs_addr, const std::string& method, std::vector<csdb::Address>& uses) {
    const auto it = known_contracts.find(abs_addr);
    if (it != known_contracts.cend()) {
        if (it->second.uses.empty() && it->second.payable == PayableStatus::Unknown) {
            csdb::Transaction t = get_transaction(it->second.ref_deploy);
            if (t.is_valid()) {
                auto maybe_invoke_info = get_smart_contract_impl(t);
                if (maybe_invoke_info.has_value()) {
                    // try update it->second.uses, make a call to ApiExec
                    if (!update_metadata(maybe_invoke_info.value(), it->second)) {
                        // disable execution until metadata can be updated
                        cslog() << kLogPrefix << "disable contract execution until connection to executor restored";
                        execution_allowed = false;
                    }
                }
            }
        }

        for (const auto& [meth, subcalls] : it->second.uses) {
            if (meth != method) {
                continue;
            }
            for (const auto& [subaddr, submeth] : subcalls) {
                if (std::find(uses.cbegin(), uses.cend(), subaddr) != uses.cend()) {
                    continue;  // skip, already in uses
                }
                uses.emplace_back(subaddr);
                add_uses_from(subaddr, submeth, uses);
            }
        }
    }
}

std::string SmartContracts::print_executed_method(const SmartContractRef& ref) {
    csdb::Transaction t = get_transaction(ref);
    if (!t.is_valid()) {
        return std::string();
    }
    if (is_executable(t)) {
        const auto maybe_invoke_info = get_smart_contract_impl(t);
        if (!maybe_invoke_info.has_value()) {
            return std::string();
        }
        const auto& invoke_info = maybe_invoke_info.value();
        if (invoke_info.method.empty()) {
            return std::string("constructor()");
        }
        std::ostringstream os;
        os << invoke_info.method << '(';
        size_t cnt_params = 0;
        for (const auto& p : invoke_info.params) {
            if (cnt_params > 0) {
                os << ',';
            }
            print(os, p);
            ++cnt_params;
        }
        os << ')';
        return os.str();
    }
    if (is_payable_target(t)) {
        // cuurently, the 2nd arg is user_field[1]
        std::string arg = t.user_field(1).value<std::string>();
        if (arg.empty()) {
            arg = "<empty>";
        }
        std::ostringstream os;
        os << PayableName << "(" << PayableArg0 << " = " << t.amount().to_double() << ", bundle = " << arg << ')';
        return os.str();
    }
    return std::string("???");
}

std::string SmartContracts::get_executed_method_name(const SmartContractRef& ref) {
    csdb::Transaction t = get_transaction(ref);
    if (!t.is_valid()) {
        return std::string();
    }
    if (is_executable(t)) {
        const auto maybe_invoke_info = get_smart_contract_impl(t);
        if (!maybe_invoke_info.has_value()) {
            return std::string();
        }
        const auto& invoke_info = maybe_invoke_info.value();
        if (invoke_info.method.empty()) {
            return std::string("constructor");
        }
        return invoke_info.method;
    }
    if (is_payable_target(t)) {
        return PayableName;
    }
    return std::string();
}

csdb::Amount SmartContracts::smart_round_fee(const csdb::Pool& block) {
    csdb::Amount fee(0);
    if (block.transactions_count() > 0) {
        for (const auto& t : block.transactions()) {
            fee += csdb::Amount(t.counted_fee().to_double());
        }
    }
    return fee;
}

void SmartContracts::update_status(QueueItem& item, cs::RoundNumber r, SmartContractStatus status) {
    item.status = status;

    switch (status) {
        case SmartContractStatus::Waiting:
            item.seq_enqueue = r;
            csdebug() << kLogPrefix << '{' << item.seq_enqueue << ".*} is waiting from #" << r;
            break;
        case SmartContractStatus::Running:
            item.seq_start = r;
            update_lock_status(item, true);
            csdebug() << kLogPrefix << '{' << item.seq_enqueue << ".*} is running from #" << r;
            break;
        case SmartContractStatus::Finished:
            item.seq_finish = r;
            csdebug() << kLogPrefix << '{' << item.seq_enqueue << ".*} is finished on #" << r;
            break;
        case SmartContractStatus::Closed:
            update_lock_status(item, false);
            csdebug() << kLogPrefix << '{' << item.seq_enqueue << ".*} is closed";
            break;
        default:
            break;
    }
}

void SmartContracts::test_contracts_locks() {
    // lookup running items
    if (!exe_queue.empty()) {
        for (const auto& exe_item : exe_queue) {
            if (exe_item.status == SmartContractStatus::Running || exe_item.status == SmartContractStatus::Finished) {
                return;
            }
        }
    }
    // no running items, ensure no locked contracts
    for (auto& item : known_contracts) {
        if (item.second.is_locked) {
            item.second.is_locked = false;
            const cs::PublicKey& key = item.first.public_key();
            csdebug() << kLogPrefix << "find locked contract " << EncodeBase58(key.data(), key.data() + key.size()) << " which is not executed now, unlock";
        }
    }
}

void SmartContracts::update_lock_status(const csdb::Address& abs_addr, bool value) {
    auto it = known_contracts.find(abs_addr);
    if (it != known_contracts.end()) {
        if (it->second.is_locked != value) {
            const cs::PublicKey& key = abs_addr.public_key();
            csdebug() << kLogPrefix << (value ? "lock" : "unlock") << " contract " << EncodeBase58(key.data(), key.data() + key.size());
            it->second.is_locked = value;
        }
    }
}

}  // namespace cs
