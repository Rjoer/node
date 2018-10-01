#include "CollectState.h"
#include "../SolverContext.h"
#include "../Node.h"

#include <iostream>

namespace slv2
{
    void CollectState::on(SolverContext& context)
    {
        auto cur_round = context.round();
        if(cur_round == 1) {
            if(Consensus::Log) {
                std::cout << name() << ": at the 1st round schedule switch to write state after " << Consensus::T_round << " msec" << std::endl;
            }
            
            SolverContext * pctx = &context;
            tag_timeout = context.scheduler().InsertOnce(Consensus::T_round, [pctx]() {
                pctx->become_writer();
            }, true); // last true - replace existing call if any
            
            return;
        }
        // on the start of the second round someone has to send TL, CollectState is always single in the network, so we send TL
        // also, if we have unsent transactions from previous rounds, also can send them
        if(cur_round == 2) {
            do_send_tl(context, cur_round - 1);
        }
        if(Consensus::Log) {
            std::cout << name() << ": starting to collect transactions" << std::endl;
        }
    }

    void CollectState::onRoundEnd(SolverContext & context)
    {
        if(tag_timeout != CallsQueueScheduler::no_tag) {
            context.scheduler().Remove(tag_timeout);
            tag_timeout = CallsQueueScheduler::no_tag;
        }
        do_send_tl(context, context.round());
    }

    Result CollectState::onTransaction(SolverContext& context, const csdb::Transaction & tr)
    {
        constexpr size_t logging_counter = 20;
        if(tr.is_valid())
        {
            ++cnt_transactions;

            if(!context.is_spammer()) {
                if(context.verify(tr)) {
                    pool.add_transaction(tr);
                    if(Consensus::Log && ((cnt_transactions % logging_counter) == 0)) {
                        std::cout << name() << ": transaction accepted (1) x" << logging_counter << std::endl;
                    }
                }
                else {
                    if(Consensus::Log) {
                        std::cout << name() << ": wrong transaction signature" << std::endl;
                    }
                }
            }
            else {
                pool.add_transaction(tr);
                //if(Consensus::Log && ((cnt_transactions % logging_counter) == 0)) {
                //    std::cout << name() << ": spammer transaction accepted (1) x" << logging_counter << std::endl;
                //}
            }
        }
        else {
            if(Consensus::Log) {
                std::cout << name() << ": invalid transaction received" << std::endl;
            }
        }
        return Result::Ignore;
    }

    Result CollectState::onTransactionList(SolverContext& /*context*/, const csdb::Pool& tl)
    {
        if(Consensus::Log) {
            std::cout << name() << ": transaction list received (cnt " << tl.transactions_count() << "), ignored" << std::endl;
        }
        return Result::Ignore;
    }

    void CollectState::do_send_tl(SolverContext& context, uint64_t sequence)
    {
        if(Consensus::Log) {
            std::cout << name() << ": sending transaction list #" <<  sequence << " of " << pool.transactions_count() << " items" << std::endl;
        }
        pool.set_sequence(sequence);
        context.node().sendTransactionList(pool);
        pool.clear();
        // ����� �������� pool ���������� � solver.v1, ������, ��� �������� :-)
        pool = csdb::Pool {};
    }

} // slv2
