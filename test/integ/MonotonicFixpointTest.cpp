/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include <algorithm>
#include <cstring>
#include <functional>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <iostream>
#include <memory>
#include <sstream>

#include "ControlFlow.h"
#include "DexClass.h"
#include "DexInstruction.h"
#include "DexLoader.h"
#include "DexUtil.h"
#include "IRInstruction.h"
#include "RedexContext.h"
#include "Transform.h"

#include "FixpointIterators.h"
#include "HashedSetAbstractDomain.h"

/*
 * The abstract domain for liveness is just the powerset domain of registers,
 * which we represent here as strings for simplicity.
 */
using LivenessDomain = HashedSetAbstractDomain<std::string>;

using namespace std::placeholders;

class IRFixpointIterator final
    : public MonotonicFixpointIterator<Block*, LivenessDomain> {
 public:
  // In the IR a CFG node is a basic block, i.e., a Block structure. A node id
  // is simply a pointer to a Block.
  using NodeId = Block*;

  // Liveness is a backward analysis, hence we apply the generic fixpoint
  // iterator by using the exit block as the root and swapping the successors
  // and predecessors functions.
  IRFixpointIterator(const ControlFlowGraph& cfg, NodeId exit_block)
      : MonotonicFixpointIterator(exit_block,
                                  std::bind(&Block::preds, _1),
                                  std::bind(&Block::succs, _1)),
        m_cfg(cfg) {}

  void analyze_node(const NodeId& block,
                    LivenessDomain* current_state) const override {
    // Since liveness is a backward analysis, we analyze each instruction inside
    // a block in the reverse order of execution.
    for (auto it = block->rbegin(); it != block->rend(); ++it) {
      if (it->type == MFLOW_OPCODE) {
        analyze_instruction(it->insn, current_state);
      }
    }
  }

  LivenessDomain analyze_edge(
      const NodeId& source_block,
      const NodeId& target_block,
      const LivenessDomain& exit_state_at_source) const override {
    // Edges have no semantic transformers attached.
    return exit_state_at_source;
  }

  void analyze_instruction(const IRInstruction* insn,
                           LivenessDomain* current_state) const {
    // This is the standard semantic definition of liveness.
    if (insn->dests_size()) {
      // The destination register of an instruction is dead.
      current_state->remove(get_register(insn->dest()));
    }
    for (size_t i = 0; i < insn->srcs_size(); ++i) {
      // The source registers of an instruction are live.
      current_state->add(get_register(insn->src(i)));
    }
    // `invoke-range` instructions require separate processing, because the
    // source registers are encoded as a range.
    if (opcode::has_range(insn->opcode())) {
      for (size_t i = 0; i < insn->range_size(); i++) {
        current_state->add(get_register(insn->range_base() + i));
      }
    }
  }

  LivenessDomain get_live_in_vars_at(const NodeId& block) const {
    // Since we performed a backward analysis by reversing the control-flow
    // graph, the set of live variables upon entering a block is given by
    // the exit state at that block.
    return get_exit_state_at(block);
  }

  LivenessDomain get_live_out_vars_at(const NodeId& block) const {
    // Similarly, the set of live variables upon exiting a block is given by
    // the entry state at that block.
    return get_entry_state_at(block);
  }

 private:
  std::string get_register(size_t i) const {
    std::ostringstream ss;
    ss << "v" << i;
    return ss.str();
  }

  const ControlFlowGraph& m_cfg;
};

/*
 *
 */
TEST(MonotonicFixpointTest, livenessAnalysis) {
  g_redex = new RedexContext();

  const char* dexfile = std::getenv("dexfile");
  ASSERT_NE(nullptr, dexfile);

  std::vector<DexStore> stores;
  DexMetadata dm;
  dm.set_id("classes");
  DexStore root_store(dm);
  root_store.add_classes(load_classes_from_dex(dexfile));
  DexClasses& classes = root_store.get_dexen().back();
  stores.emplace_back(std::move(root_store));
  std::cout << "Loaded classes: " << classes.size() << std::endl;

  for (const auto& cls : classes) {
    if (std::strcmp(cls->get_name()->c_str(),
                    "Lcom/facebook/redextest/MonotonicFixpoint;") == 0) {
      for (const auto& method : cls->get_vmethods()) {
        if (std::strcmp(method->get_name()->c_str(), "function_1") == 0) {
          auto code = method->get_code();
          code->build_cfg();
          ControlFlowGraph& cfg = code->cfg();
          std::cout << "CFG of function_1:" << std::endl
                    << SHOW(cfg) << std::endl;
          auto it = std::find_if(cfg.blocks().begin(),
                                 cfg.blocks().end(),
                                 [](Block* b) { return b->id() == 2; });
          ASSERT_TRUE(it != cfg.blocks().end());
          IRFixpointIterator fixpoint_iterator(cfg, *it);
          fixpoint_iterator.run(LivenessDomain());

          for (Block* block : cfg.blocks()) {
            LivenessDomain live_in =
                fixpoint_iterator.get_live_in_vars_at(block);
            LivenessDomain live_out =
                fixpoint_iterator.get_live_out_vars_at(block);
            // Checking the live in/out variables at block boundaries.
            switch (block->id()) {
            case 0: {
              EXPECT_EQ(0, live_in.size());
              EXPECT_THAT(live_out.elements(),
                          ::testing::UnorderedElementsAre("v0", "v2"));
              break;
            }
            case 1: {
              EXPECT_THAT(live_in.elements(),
                          ::testing::UnorderedElementsAre("v0", "v2"));
              EXPECT_THAT(live_out.elements(),
                          ::testing::UnorderedElementsAre("v0", "v2"));
              break;
            }
            case 2: {
              EXPECT_THAT(live_in.elements(), ::testing::ElementsAre("v2"));
              EXPECT_EQ(0, live_out.size());
              break;
            }
            default: { FAIL() << "Unexpected block"; }
            }

            // Checking the live in/out variables at position instructions.
            for (auto it = block->rbegin(); it != block->rend(); ++it) {
              if (it->type == MFLOW_OPCODE) {
                // We replay the analysis of a block backwards starting from the
                // exit state (the set of live-out variables).
                fixpoint_iterator.analyze_instruction(it->insn, &live_out);
              }
              if (it->type == MFLOW_POSITION) {
                switch (it->pos->line) {
                case 48: {
                  EXPECT_THAT(live_out.elements(),
                              ::testing::UnorderedElementsAre("v0", "v2"));
                  break;
                }
                case 49:
                case 50: {
                  EXPECT_THAT(live_out.elements(),
                              ::testing::UnorderedElementsAre("v1", "v2"));
                  break;
                }
                case 51: {
                  EXPECT_THAT(live_out.elements(),
                              ::testing::UnorderedElementsAre("v0", "v2"));
                  break;
                }
                }
              }
            }
          }
        }
      }
    }

    delete g_redex;
  }
}
