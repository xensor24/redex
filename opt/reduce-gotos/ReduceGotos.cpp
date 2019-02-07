/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/*
 * This optimizer pass reduces goto instructions.
 *
 * It does so in two ways:
 * 1) When a conditional branch would fallthrough to a block that has multiple
 *    sources, and the branch target only one has one, invert condition and
 *    swap branch and goto target. This reduces the need for additional gotos /
 *    maximizes the fallthrough efficiency.
 * 2) It replaces gotos that eventually simply return by return instructions.
 *    Return instructions tend to have a smaller encoding than goto
 *    instructions, and tend to compress better due to less entropy (no offset).
 */

#include "ReduceGotos.h"

#include <vector>

#include "ControlFlow.h"
#include "DexClass.h"
#include "DexUtil.h"
#include "IRCode.h"
#include "IRInstruction.h"
#include "IROpcode.h"
#include "Walkers.h"

namespace {

constexpr const char* METRIC_GOTOS_REPLACED_WITH_RETURNS =
    "num_gotos_replaced_with_returns";
constexpr const char* METRIC_TRAILING_MOVES_REMOVED =
    "num_trailing_moves_removed";
constexpr const char* METRIC_INVERTED_CONDITIONAL_BRANCHES =
    "num_inverted_conditional_branches";

} // namespace

ReduceGotosPass::Stats ReduceGotosPass::process_code(IRCode* code) {
  Stats stats;

  code->build_cfg(/* editable = true*/);
  auto& cfg = code->cfg();

  // Optimization #1: Invert conditional branch conditions and swap goto/branch
  // targets if this may lead to more fallthrough cases where no additional
  // goto instruction is needed
  for (cfg::Block* b : cfg.blocks()) {
    auto br = b->branchingness();
    if (br != opcode::BRANCH_IF) {
      continue;
    }

    // So we have a block that ends with a conditional branch
    // Let's find the (unique) branch and goto targets
    auto it = b->get_last_insn();
    always_assert(it != b->end());
    auto insn = it->insn;
    auto opcode = insn->opcode();
    always_assert(is_conditional_branch(opcode));
    cfg::Edge* goto_edge = cfg.get_succ_edge_of_type(b, cfg::EDGE_GOTO);
    cfg::Edge* branch_edge = cfg.get_succ_edge_of_type(b, cfg::EDGE_BRANCH);
    always_assert(goto_edge != nullptr);
    always_assert(branch_edge != nullptr);

    // If beneficial, invert condition and swap targets
    if (goto_edge->target()->preds().size() > 1 &&
        branch_edge->target()->preds().size() == 1) {
      stats.inverted_conditional_branches++;
      // insert condition
      insn->set_opcode(opcode::invert_conditional_branch(opcode));
      // swap goto and branch target
      cfg::Block* branch_target = branch_edge->target();
      cfg::Block* goto_target = goto_edge->target();
      cfg.set_edge_target(branch_edge, goto_target);
      cfg.set_edge_target(goto_edge, branch_target);
    }
  }

  // Optimization #2:
  // Inline all blocks that just contain a single return instruction and are
  // reached via a goto edge; this may leave behind some unreachable blocks
  // which will get cleaned up via simplify() eventually.
  // Small bonus optimization: Also eliminate move instructions that only exist
  // to faciliate shared return instructions.

  auto order = cfg.order();
  bool rerun;
  do {
    rerun = false;
    std::vector<cfg::Edge*> edges_to_delete;
    for (auto block_it = order.begin(); block_it != order.end(); ++block_it) {
      cfg::Block* b = *block_it;
      auto last_mie_it = b->get_last_insn();
      if (last_mie_it == b->end()) {
        continue;
      }
      auto first_mie_it = b->get_first_insn();
      if (first_mie_it != last_mie_it) {
        continue;
      }
      MethodItemEntry* mie = &*last_mie_it;
      if (!is_return(mie->insn->opcode())) {
        continue;
      }

      const auto& preds = b->preds();
      for (cfg::Edge* e : preds) {
        if (e->type() != cfg::EDGE_GOTO) {
          continue;
        }

        cfg::Block* src = e->src();

        MethodItemEntryCloner cloner;
        MethodItemEntry* cloned_mie = cloner.clone(mie);

        bool removed_trailing_move = false;
        if (cloned_mie->insn->srcs_size()) {
          // eliminate trailing move instruction by specializing return
          // instruction
          auto src_last_mie_it = src->get_last_insn();
          if (src_last_mie_it != src->end()) {
            // We are looking for an instruction of the form
            //   move $dest, $source
            // matching an
            //   return $dest
            // instruction we found earlier.
            IRInstruction* src_last_insn = src_last_mie_it->insn;
            IRInstruction* cloned_insn = cloned_mie->insn;
            if (is_move(src_last_insn->opcode()) &&
                src_last_insn->dest() == cloned_insn->src(0) &&
                src_last_insn->is_wide() == cloned_insn->is_wide()) {
              // Found a matching move! We'll rewrite the (cloned) return
              // instruction to
              //   return $source
              removed_trailing_move = true;
              cloned_insn->set_src(0, src_last_insn->src(0));
              src->remove_opcode(src_last_mie_it);
              stats.removed_trailing_moves++;
            }
          }
        }

        if (removed_trailing_move) {
          // Let's remember to run the optimization one more time, as removing
          // this move instruction may have unlocked further potential as it may
          // create a block with just a return in it.
          rerun = true;
        } else if (block_it != order.begin() && *std::prev(block_it) == src) {
          // Don't put in a return instruction if we would just fall through
          // anyway, i.e. if linearization won't insert a goto here.
          continue;
        }

        src->push_back(*cloned_mie);
        edges_to_delete.push_back(e);
      }
    }

    for (cfg::Edge* e : edges_to_delete) {
      cfg.delete_edge(e);
      stats.replaced_gotos_with_returns++;
    }
  } while (rerun);

  code->clear_cfg();
  return stats;
}

void ReduceGotosPass::run_pass(DexStoresVector& stores,
                               ConfigFiles& /* unused */,
                               PassManager& mgr) {
  auto scope = build_class_scope(stores);

  Stats stats = walk::parallel::reduce_methods<Stats>(
      scope,
      [](DexMethod* method) -> Stats {
        const auto code = method->get_code();
        if (!code) {
          return Stats{};
        }

        Stats stats = ReduceGotosPass::process_code(code);
        if (stats.replaced_gotos_with_returns ||
            stats.inverted_conditional_branches) {
          TRACE(RG, 3,
                "[reduce gotos] Replaced %u gotos with returns, "
                "removed %u trailing moves, "
                "inverted %u conditional branches in {%s}\n",
                stats.replaced_gotos_with_returns, stats.removed_trailing_moves,
                stats.inverted_conditional_branches, SHOW(method));
        }
        return stats;
      },
      [](Stats a, Stats b) {
        Stats c;
        c.replaced_gotos_with_returns =
            a.replaced_gotos_with_returns + b.replaced_gotos_with_returns;
        c.removed_trailing_moves =
            a.removed_trailing_moves + b.removed_trailing_moves;
        c.inverted_conditional_branches =
            a.inverted_conditional_branches + b.inverted_conditional_branches;
        return c;
      });

  mgr.incr_metric(METRIC_GOTOS_REPLACED_WITH_RETURNS,
                  stats.replaced_gotos_with_returns);
  mgr.incr_metric(METRIC_TRAILING_MOVES_REMOVED, stats.removed_trailing_moves);
  mgr.incr_metric(METRIC_INVERTED_CONDITIONAL_BRANCHES,
                  stats.inverted_conditional_branches);
  TRACE(RG, 1,
        "[reduce gotos] Replaced %u gotos with returns, inverted %u "
        "conditional brnaches in total\n",
        stats.replaced_gotos_with_returns, stats.inverted_conditional_branches);
}

static ReduceGotosPass s_pass;
