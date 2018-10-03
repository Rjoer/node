#include "SolverCore.h"
#include <Solver/Solver.hpp>
#include "Node.h"
#include <Solver/Generals.hpp>
#include <csdb/currency.h>

#include <iostream>

namespace slv2
{

    const Credits::HashVector& SolverCore::getMyVector() const
    {
        if(opt_is_proxy_v1 && pslv_v1) {
            return pslv_v1->getMyVector();
        }
        if(!pown_hvec) {
            // empty one is for test purpose
            static Credits::HashVector stub {};
            return stub;
        }
        return *pown_hvec;
    }

    const Credits::HashMatrix& SolverCore::getMyMatrix() const
    {
        if(opt_is_proxy_v1 && pslv_v1) {
            return pslv_v1->getMyMatrix();
        }
        if(!pgen) {
            // empty one is for test purpose
            static Credits::HashMatrix stub {};
            return stub;
        }
        return pgen->getMatrix();
    }

    void SolverCore::set_keys(const KeyType& pub, const KeyType& priv)
    {
        if(opt_is_proxy_v1 && pslv_v1) {
            pslv_v1->set_keys(pub, priv);
        }
        public_key = pub;
        private_key = priv;
        // "autostart" in node environment
        if(is_finished()) {
            start();
        }
    }

    void SolverCore::addInitialBalance()
    {
        if(opt_is_proxy_v1 && pslv_v1) {
            pslv_v1->addInitialBalance();
            return;
        }

        // copied from original solver-1
        //std::cout << "===SETTING DB===" << std::endl;
        const std::string start_address = "0000000000000000000000000000000000000000000000000000000000000002";
        csdb::Transaction tr;
        tr.set_target(csdb::Address::from_public_key((char*) public_key.data()));
        tr.set_source(csdb::Address::from_string(start_address));
        tr.set_currency(csdb::Currency("CS"));
        tr.set_amount(csdb::Amount(10000, 0));
        tr.set_balance(csdb::Amount(10000000, 0));
        tr.set_innerID(1);
        send_wallet_transaction(tr);
        if(Consensus::Log) {
            std::cout << "SolverCore: initial balance added" << std::endl;
        }
    }

    void SolverCore::setBigBangStatus(bool status)
    {
        if(opt_is_proxy_v1 && pslv_v1) {
            pslv_v1->setBigBangStatus(status);
            return;
        }

        if(!pstate) {
            return;
        }
        if(status) {
            handleTransitions(Event::BigBang);
        }
    }

    void SolverCore::gotTransaction(const csdb::Transaction& trans)
    {
        if(opt_is_proxy_v1 && pslv_v1) {
            csdb::Transaction tmp = trans;
            pslv_v1->gotTransaction(std::move(tmp));
            return;
        }

        if(!pstate) {
            return;
        }
        // produces too much output:
        //if(Consensus::Log) {
        //    std::cout << "SolverCore: gotTransaction()" << std::endl;
        //}
        if(stateCompleted(pstate->onTransaction(*pcontext, trans))) {
            handleTransitions(Event::Transactions);
        }
    }

    void SolverCore::gotTransactionList(csdb::Pool& p)
    {
        if(opt_is_proxy_v1 && pslv_v1) {
            csdb::Pool tmp = p;
            pslv_v1->gotTransactionList(std::move(tmp));
            return;
        }

        auto tl_seq = p.sequence();
        if(tl_seq == last_trans_list_recv) {
            // already received
            if(Consensus::Log) {
                std::cout << "SolverCore: transaction list (#" << tl_seq << ") already received, ignore" << std::endl;
            }
            return;
        }
        last_trans_list_recv = tl_seq;

        // ������ ��� ������ ������
        pool = csdb::Pool {};

        if(Consensus::Log) {
            std::cout << "SolverCore: transaction list (#" << tl_seq << ") received, updating own hashvector" << std::endl;
        }
        // bad tansactions storage:
        csdb::Pool b_pool {};
        // update own hash vector
        if(pnode != nullptr && pgen != nullptr) {
            auto result = pgen->buildvector(p, pool, pnode->getConfidants().size(), b_pool);
            pown_hvec->Sender = pnode->getMyConfNumber();
            pown_hvec->hash = result;
        }

        if(!pstate) {
            return;
        }
        if(stateCompleted(pstate->onTransactionList(*pcontext, p))) {
            handleTransitions(Event::Transactions);
        }
    }

    void SolverCore::gotVector(const Credits::HashVector& vect)
    {
        if(opt_is_proxy_v1 && pslv_v1) {
            Credits::HashVector tmp = vect;
            pslv_v1->gotVector(std::move(tmp));
            return;
        }

        if(!pstate) {
            return;
        }
        //TODO: how to get real public key from vect.Sender?
        if(Consensus::Log) {
            std::cout << "SolverCore: gotVector()" << std::endl;
        }
        if(stateCompleted(pstate->onVector(*pcontext, vect, PublicKey {}))) {
            handleTransitions(Event::Vectors);
        }
    }

    void SolverCore::gotMatrix(const Credits::HashMatrix& matr)
    {
        if(opt_is_proxy_v1 && pslv_v1) {
            Credits::HashMatrix tmp = matr;
            pslv_v1->gotMatrix(std::move(tmp));
            return;
        }

        if(!pstate) {
            return;
        }
        //TODO: how to get real public key from vect.Sender?
        if(Consensus::Log) {
            std::cout << "SolverCore: gotMatrix()" << std::endl;
        }
        if(stateCompleted(pstate->onMatrix(*pcontext, matr, PublicKey {}))) {
            handleTransitions(Event::Matrices);
        }
    }

    void SolverCore::gotBlock(csdb::Pool& p, const PublicKey& sender)
    {
        if(opt_is_proxy_v1 && pslv_v1) {
            csdb::Pool tmp = p;
            pslv_v1->gotBlock(std::move(tmp), sender);
            return;
        }

        if(!pstate) {
            return;
        }
        if(Consensus::Log) {
            std::cout << "SolverCore: gotBlock()" << std::endl;
        }
        if(stateCompleted(pstate->onBlock(*pcontext, p, sender))) {
            handleTransitions(Event::Block);
        }
    }

    void SolverCore::gotBlockRequest(const csdb::PoolHash& p_hash, const PublicKey& sender)
    {
        if(opt_is_proxy_v1 && pslv_v1) {
            csdb::PoolHash tmp = p_hash;
            pslv_v1->gotBlockRequest(std::move(tmp), sender);
            return;
        }

        if(Consensus::Log) {
            std::cout << "SolverCore: gotBlockRequest()" << std::endl;
        }
        // state does not take part
        if(pnode != nullptr) {
            csdb::Pool p = pnode->getBlockChain().loadBlock(p_hash);
            if(p.is_valid()) {
                p.set_previous_hash(csdb::PoolHash::from_string(""));
                pnode->sendBlockReply(p, sender);
            }
        }
    }

    void SolverCore::gotBlockReply(csdb::Pool& p)
    {
        if(opt_is_proxy_v1 && pslv_v1) {
            pslv_v1->gotBlockReply(std::move(p));
            return;
        }

        if(Consensus::Log) {
            std::cout << "SolverCore: gotBlockReply()" << std::endl;
        }
        //std::cout << "Solver -> Got Block for my Request: " << p.sequence() << std::endl;
        if(p.sequence() == pnode->getBlockChain().getLastWrittenSequence() + 1) {
            pnode->getBlockChain().putBlock(p);
        }
    }

    void SolverCore::gotHash(const Hash& hash, const PublicKey& sender)
    {
        if(opt_is_proxy_v1 && pslv_v1) {
            pslv_v1->gotHash(hash, sender);
            return;
        }

        if(!pstate) {
            return;
        }
        if(Consensus::Log) {
            std::cout << "SolverCore: gotHash()" << std::endl;
        }
        if(stateCompleted(pstate->onHash(*pcontext, hash, sender))) {
            handleTransitions(Event::Hashes);
        }
    }

    void SolverCore::addConfirmation(uint8_t own_conf_number)
    {
        if(opt_is_proxy_v1 && pslv_v1) {
            pslv_v1->addConfirmation(own_conf_number);
            return;
        }

        if(Consensus::Log) {
            std::cout << "SolverCore: addConfirmation(): not implemented yet" << std::endl;
        }
        if(!pstate) {
            return;
        }
    }

    void SolverCore::beforeNextRound()
    {
        if(opt_is_proxy_v1 && pslv_v1) {
            pslv_v1->beforeNextRound();
            return;
        }
        
        if(!pstate) {
            return;
        }
        pstate->onRoundEnd(*pcontext);
    }

    void SolverCore::nextRound()
    {
        if(opt_is_proxy_v1 && pslv_v1) {
            pslv_v1->nextRound();
            return;
        }

        // as store result of current round:
        if(Consensus::Log) {
            std::cout << "SolverCore: clear all stored senders (vectors, matrices, hashes)" << std::endl;
        }

        recv_vect.clear();
        recv_matr.clear();
        recv_hash.clear();

        if(!pstate) {
            return;
        }
        if(pnode != nullptr) {
            cur_round = pnode->getRoundNumber();
        }
        if(Consensus::Log) {
            std::cout << "SolverCore: nextRound()" << std::endl;
        }
        if(stateCompleted(pstate->onRoundTable(*pcontext, cur_round))) {
            handleTransitions(Event::RoundTable);
        }
    }

    void SolverCore::send_wallet_transaction(const csdb::Transaction& tr)
    {
        if(opt_is_proxy_v1 && pslv_v1) {
            pslv_v1->send_wallet_transaction(tr);
            return;
        }

        // thread-safe with flushTransactions(), suppose to receive calls from network-related threads
        std::lock_guard<std::mutex> l(trans_mtx);
        transactions.push_back(tr);
    }

} // slv2
