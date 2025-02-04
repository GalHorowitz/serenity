/*
 * Copyright (c) 2021, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/QuickSort.h>
#include <AK/RedBlackTree.h>
#include <AK/Stack.h>
#include <LibRegex/Regex.h>
#include <LibRegex/RegexBytecodeStreamOptimizer.h>

namespace regex {

using Detail::Block;

template<typename Parser>
void Regex<Parser>::run_optimization_passes()
{
    parser_result.bytecode.flatten();

    // Rewrite fork loops as atomic groups
    // e.g. a*b -> (ATOMIC a*)b
    attempt_rewrite_loops_as_atomic_groups(split_basic_blocks());

    parser_result.bytecode.flatten();
}

template<typename Parser>
typename Regex<Parser>::BasicBlockList Regex<Parser>::split_basic_blocks()
{
    BasicBlockList block_boundaries;
    auto& bytecode = parser_result.bytecode;
    size_t end_of_last_block = 0;

    MatchState state;
    state.instruction_position = 0;
    auto check_jump = [&]<typename T>(OpCode const& opcode) {
        auto& op = static_cast<T const&>(opcode);
        ssize_t jump_offset = op.size() + op.offset();
        if (jump_offset >= 0) {
            block_boundaries.append({ end_of_last_block, state.instruction_position });
            end_of_last_block = state.instruction_position + opcode.size();
        } else {
            // This op jumps back, see if that's within this "block".
            if (jump_offset + state.instruction_position > end_of_last_block) {
                // Split the block!
                block_boundaries.append({ end_of_last_block, jump_offset + state.instruction_position });
                block_boundaries.append({ jump_offset + state.instruction_position, state.instruction_position });
                end_of_last_block = state.instruction_position + opcode.size();
            } else {
                // Nope, it's just a jump to another block
                block_boundaries.append({ end_of_last_block, state.instruction_position });
                end_of_last_block = state.instruction_position + opcode.size();
            }
        }
    };
    for (;;) {
        auto& opcode = bytecode.get_opcode(state);

        switch (opcode.opcode_id()) {
        case OpCodeId::Jump:
            check_jump.template operator()<OpCode_Jump>(opcode);
            break;
        case OpCodeId::JumpNonEmpty:
            check_jump.template operator()<OpCode_JumpNonEmpty>(opcode);
            break;
        case OpCodeId::ForkJump:
            check_jump.template operator()<OpCode_ForkJump>(opcode);
            break;
        case OpCodeId::ForkStay:
            check_jump.template operator()<OpCode_ForkStay>(opcode);
            break;
        case OpCodeId::FailForks:
            block_boundaries.append({ end_of_last_block, state.instruction_position });
            end_of_last_block = state.instruction_position + opcode.size();
            break;
        case OpCodeId::Repeat: {
            // Repeat produces two blocks, one containing its repeated expr, and one after that.
            auto repeat_start = state.instruction_position - static_cast<OpCode_Repeat const&>(opcode).offset();
            if (repeat_start > end_of_last_block)
                block_boundaries.append({ end_of_last_block, repeat_start });
            block_boundaries.append({ repeat_start, state.instruction_position });
            end_of_last_block = state.instruction_position + opcode.size();
            break;
        }
        default:
            break;
        }

        auto next_ip = state.instruction_position + opcode.size();
        if (next_ip < bytecode.size())
            state.instruction_position = next_ip;
        else
            break;
    }

    if (end_of_last_block < bytecode.size())
        block_boundaries.append({ end_of_last_block, bytecode.size() });

    quick_sort(block_boundaries, [](auto& a, auto& b) { return a.start < b.start; });

    return block_boundaries;
}

static bool block_satisfies_atomic_rewrite_precondition(ByteCode const& bytecode, Block const& repeated_block, Block const& following_block)
{
    Vector<Vector<CompareTypeAndValuePair>> repeated_values;
    HashTable<size_t> active_capture_groups;
    MatchState state;
    for (state.instruction_position = repeated_block.start; state.instruction_position < repeated_block.end;) {
        auto& opcode = bytecode.get_opcode(state);
        switch (opcode.opcode_id()) {
        case OpCodeId::Compare: {
            auto compares = static_cast<OpCode_Compare const&>(opcode).flat_compares();
            if (repeated_values.is_empty() && any_of(compares, [](auto& compare) { return compare.type == CharacterCompareType::AnyChar; }))
                return false;
            repeated_values.append(move(compares));
            break;
        }
        case OpCodeId::CheckBegin:
        case OpCodeId::CheckEnd:
            if (repeated_values.is_empty())
                return true;
            break;
        case OpCodeId::CheckBoundary:
            // FIXME: What should we do with these? for now, let's fail.
            return false;
        case OpCodeId::Restore:
        case OpCodeId::GoBack:
            return false;
        case OpCodeId::SaveRightCaptureGroup:
            active_capture_groups.set(static_cast<OpCode_SaveRightCaptureGroup const&>(opcode).id());
            break;
        case OpCodeId::SaveLeftCaptureGroup:
            active_capture_groups.set(static_cast<OpCode_SaveLeftCaptureGroup const&>(opcode).id());
            break;
        default:
            break;
        }

        state.instruction_position += opcode.size();
    }
    dbgln_if(REGEX_DEBUG, "Found {} entries in reference", repeated_values.size());
    dbgln_if(REGEX_DEBUG, "Found {} active capture groups", active_capture_groups.size());

    // Find the first compare in the following block, it must NOT match any of the values in `repeated_values'.
    for (state.instruction_position = following_block.start; state.instruction_position < following_block.end;) {
        auto& opcode = bytecode.get_opcode(state);
        switch (opcode.opcode_id()) {
        // Note: These have to exist since we're effectively repeating the following block as well
        case OpCodeId::SaveRightCaptureGroup:
            active_capture_groups.set(static_cast<OpCode_SaveRightCaptureGroup const&>(opcode).id());
            break;
        case OpCodeId::SaveLeftCaptureGroup:
            active_capture_groups.set(static_cast<OpCode_SaveLeftCaptureGroup const&>(opcode).id());
            break;
        case OpCodeId::Compare: {
            // We found a compare, let's see what it has.
            auto compares = static_cast<OpCode_Compare const&>(opcode).flat_compares();
            if (compares.is_empty())
                break;

            if (any_of(compares, [&](auto& compare) {
                    return compare.type == CharacterCompareType::AnyChar
                        || (compare.type == CharacterCompareType::Reference && active_capture_groups.contains(compare.value));
                }))
                return false;

            for (auto& repeated_value : repeated_values) {
                // FIXME: This is too naive!
                if (any_of(repeated_value, [](auto& compare) { return compare.type == CharacterCompareType::AnyChar; }))
                    return false;

                for (auto& repeated_compare : repeated_value) {
                    // FIXME: This is too naive! it will miss _tons_ of cases since it doesn't check ranges!
                    if (any_of(compares, [&](auto& compare) { return compare.type == repeated_compare.type && compare.value == repeated_compare.value; }))
                        return false;
                }
            }
            return true;
        }
        case OpCodeId::CheckBegin:
        case OpCodeId::CheckEnd:
            return true; // Nothing can match the end!
        case OpCodeId::CheckBoundary:
            // FIXME: What should we do with these? For now, consider them a failure.
            return false;
        default:
            break;
        }

        state.instruction_position += opcode.size();
    }

    return true;
}

template<typename Parser>
void Regex<Parser>::attempt_rewrite_loops_as_atomic_groups(BasicBlockList const& basic_blocks)
{
    auto& bytecode = parser_result.bytecode;
    if constexpr (REGEX_DEBUG) {
        RegexDebug dbg;
        dbg.print_bytecode(*this);
        for (auto& block : basic_blocks)
            dbgln("block from {} to {}", block.start, block.end);
    }

    // A pattern such as:
    //     bb0       |  RE0
    //               |  ForkX bb0
    //     -------------------------
    //     bb1       |  RE1
    // can be rewritten as:
    //     loop.hdr  | ForkStay bb1
    //     -------------------------
    //     bb0       | RE0
    //               | ForkReplaceX bb0
    //     -------------------------
    //     bb1       | RE1
    // provided that first(RE1) not-in end(RE0), which is to say
    // that RE1 cannot start with whatever RE0 has matched (ever).
    //
    // Alternatively, a second form of this pattern can also occur:
    //     bb0 | *
    //         | ForkX bb2
    //     ------------------------
    //     bb1 | RE0
    //         | Jump bb0
    //     ------------------------
    //     bb2 | RE1
    // which can be transformed (with the same preconditions) to:
    //     bb0 | *
    //         | ForkReplaceX bb2
    //     ------------------------
    //     bb1 | RE0
    //         | Jump bb0
    //     ------------------------
    //     bb2 | RE1

    enum class AlternateForm {
        DirectLoopWithoutHeader, // loop without proper header, a block forking to itself. i.e. the first form.
        DirectLoopWithHeader,    // loop with proper header, i.e. the second form.
    };
    struct CandidateBlock {
        Block forking_block;
        Optional<Block> new_target_block;
        AlternateForm form;
    };
    Vector<CandidateBlock> candidate_blocks;

    auto is_an_eligible_jump = [](OpCode const& opcode, size_t ip, size_t block_start, AlternateForm alternate_form) {
        switch (opcode.opcode_id()) {
        case OpCodeId::JumpNonEmpty: {
            auto& op = static_cast<OpCode_JumpNonEmpty const&>(opcode);
            auto form = op.form();
            if (form != OpCodeId::Jump && alternate_form == AlternateForm::DirectLoopWithHeader)
                return false;
            if (form != OpCodeId::ForkJump && form != OpCodeId::ForkStay && alternate_form == AlternateForm::DirectLoopWithoutHeader)
                return false;
            return op.offset() + ip + opcode.size() == block_start;
        }
        case OpCodeId::ForkJump:
            if (alternate_form == AlternateForm::DirectLoopWithHeader)
                return false;
            return static_cast<OpCode_ForkJump const&>(opcode).offset() + ip + opcode.size() == block_start;
        case OpCodeId::ForkStay:
            if (alternate_form == AlternateForm::DirectLoopWithHeader)
                return false;
            return static_cast<OpCode_ForkStay const&>(opcode).offset() + ip + opcode.size() == block_start;
        case OpCodeId::Jump:
            // Infinite loop does *not* produce forks.
            if (alternate_form == AlternateForm::DirectLoopWithoutHeader)
                return false;
            if (alternate_form == AlternateForm::DirectLoopWithHeader)
                return static_cast<OpCode_Jump const&>(opcode).offset() + ip + opcode.size() == block_start;
            VERIFY_NOT_REACHED();
        default:
            return false;
        }
    };
    for (size_t i = 0; i < basic_blocks.size(); ++i) {
        auto forking_block = basic_blocks[i];
        Optional<Block> fork_fallback_block;
        if (i + 1 < basic_blocks.size())
            fork_fallback_block = basic_blocks[i + 1];
        MatchState state;
        // Check if the last instruction in this block is a jump to the block itself:
        {
            state.instruction_position = forking_block.end;
            auto& opcode = bytecode.get_opcode(state);
            if (is_an_eligible_jump(opcode, state.instruction_position, forking_block.start, AlternateForm::DirectLoopWithoutHeader)) {
                // We've found RE0 (and RE1 is just the following block, if any), let's see if the precondition applies.
                // if RE1 is empty, there's no first(RE1), so this is an automatic pass.
                if (!fork_fallback_block.has_value() || fork_fallback_block->end == fork_fallback_block->start) {
                    candidate_blocks.append({ forking_block, fork_fallback_block, AlternateForm::DirectLoopWithoutHeader });
                    break;
                }

                if (block_satisfies_atomic_rewrite_precondition(bytecode, forking_block, *fork_fallback_block)) {
                    candidate_blocks.append({ forking_block, fork_fallback_block, AlternateForm::DirectLoopWithoutHeader });
                    break;
                }
            }
        }
        // Check if the last instruction in the last block is a direct jump to this block
        if (fork_fallback_block.has_value()) {
            state.instruction_position = fork_fallback_block->end;
            auto& opcode = bytecode.get_opcode(state);
            if (is_an_eligible_jump(opcode, state.instruction_position, forking_block.start, AlternateForm::DirectLoopWithHeader)) {
                // We've found bb1 and bb0, let's just make sure that bb0 forks to bb2.
                state.instruction_position = forking_block.end;
                auto& opcode = bytecode.get_opcode(state);
                if (opcode.opcode_id() == OpCodeId::ForkJump || opcode.opcode_id() == OpCodeId::ForkStay) {
                    Optional<Block> block_following_fork_fallback;
                    if (i + 2 < basic_blocks.size())
                        block_following_fork_fallback = basic_blocks[i + 2];
                    if (!block_following_fork_fallback.has_value() || block_satisfies_atomic_rewrite_precondition(bytecode, *fork_fallback_block, *block_following_fork_fallback)) {
                        candidate_blocks.append({ forking_block, {}, AlternateForm::DirectLoopWithHeader });
                        break;
                    }
                }
            }
        }
    }

    dbgln_if(REGEX_DEBUG, "Found {} candidate blocks", candidate_blocks.size());
    if (candidate_blocks.is_empty()) {
        dbgln_if(REGEX_DEBUG, "Failed to find anything for {}", pattern_value);
        return;
    }

    RedBlackTree<size_t, size_t> needed_patches;

    // Reverse the blocks, so we can patch the bytecode without messing with the latter patches.
    quick_sort(candidate_blocks, [](auto& a, auto& b) { return b.forking_block.start > a.forking_block.start; });
    for (auto& candidate : candidate_blocks) {
        // Note that both forms share a ForkReplace patch in forking_block.
        // Patch the ForkX in forking_block to be a ForkReplaceX instead.
        auto& opcode_id = bytecode[candidate.forking_block.end];
        if (opcode_id == (ByteCodeValueType)OpCodeId::ForkStay) {
            opcode_id = (ByteCodeValueType)OpCodeId::ForkReplaceStay;
        } else if (opcode_id == (ByteCodeValueType)OpCodeId::ForkJump) {
            opcode_id = (ByteCodeValueType)OpCodeId::ForkReplaceJump;
        } else if (opcode_id == (ByteCodeValueType)OpCodeId::JumpNonEmpty) {
            auto& jump_opcode_id = bytecode[candidate.forking_block.end + 3];
            if (jump_opcode_id == (ByteCodeValueType)OpCodeId::ForkStay)
                jump_opcode_id = (ByteCodeValueType)OpCodeId::ForkReplaceStay;
            else if (jump_opcode_id == (ByteCodeValueType)OpCodeId::ForkJump)
                jump_opcode_id = (ByteCodeValueType)OpCodeId::ForkReplaceJump;
            else
                VERIFY_NOT_REACHED();
        } else {
            VERIFY_NOT_REACHED();
        }

        if (candidate.form == AlternateForm::DirectLoopWithoutHeader) {
            if (candidate.new_target_block.has_value()) {
                // Insert a fork-stay targeted at the second block.
                bytecode.insert(candidate.forking_block.start, (ByteCodeValueType)OpCodeId::ForkStay);
                bytecode.insert(candidate.forking_block.start + 1, candidate.new_target_block->start - candidate.forking_block.start);
                needed_patches.insert(candidate.forking_block.start, 2u);
            }
        }
    }

    if (!needed_patches.is_empty()) {
        MatchState state;
        state.instruction_position = 0;
        struct Patch {
            ssize_t value;
            size_t offset;
            bool should_negate { false };
        };
        for (;;) {
            if (state.instruction_position >= bytecode.size())
                break;

            auto& opcode = bytecode.get_opcode(state);
            Stack<Patch, 2> patch_points;

            switch (opcode.opcode_id()) {
            case OpCodeId::Jump:
                patch_points.push({ static_cast<OpCode_Jump const&>(opcode).offset(), state.instruction_position + 1 });
                break;
            case OpCodeId::JumpNonEmpty:
                patch_points.push({ static_cast<OpCode_JumpNonEmpty const&>(opcode).offset(), state.instruction_position + 1 });
                patch_points.push({ static_cast<OpCode_JumpNonEmpty const&>(opcode).checkpoint(), state.instruction_position + 2 });
                break;
            case OpCodeId::ForkJump:
                patch_points.push({ static_cast<OpCode_ForkJump const&>(opcode).offset(), state.instruction_position + 1 });
                break;
            case OpCodeId::ForkStay:
                patch_points.push({ static_cast<OpCode_ForkStay const&>(opcode).offset(), state.instruction_position + 1 });
                break;
            case OpCodeId::Repeat:
                patch_points.push({ -(ssize_t) static_cast<OpCode_Repeat const&>(opcode).offset(), state.instruction_position + 1, true });
                break;
            default:
                break;
            }

            while (!patch_points.is_empty()) {
                auto& patch_point = patch_points.top();
                auto target_offset = patch_point.value + state.instruction_position + opcode.size();

                constexpr auto do_patch = [](auto& patch_it, auto& patch_point, auto& target_offset, auto& bytecode, auto ip) {
                    if (patch_it.key() == ip)
                        return;

                    if (patch_point.value < 0 && target_offset < patch_it.key() && ip > patch_it.key())
                        bytecode[patch_point.offset] += (patch_point.should_negate ? 1 : -1) * (*patch_it);
                    else if (patch_point.value > 0 && target_offset > patch_it.key() && ip < patch_it.key())
                        bytecode[patch_point.offset] += (patch_point.should_negate ? -1 : 1) * (*patch_it);
                };

                if (auto patch_it = needed_patches.find_largest_not_above_iterator(target_offset); !patch_it.is_end())
                    do_patch(patch_it, patch_point, target_offset, bytecode, state.instruction_position);
                else if (auto patch_it = needed_patches.find_largest_not_above_iterator(state.instruction_position); !patch_it.is_end())
                    do_patch(patch_it, patch_point, target_offset, bytecode, state.instruction_position);

                patch_points.pop();
            }

            state.instruction_position += opcode.size();
        }
    }

    if constexpr (REGEX_DEBUG) {
        warnln("Transformed to:");
        RegexDebug dbg;
        dbg.print_bytecode(*this);
    }
}

void Optimizer::append_alternation(ByteCode& target, ByteCode&& left, ByteCode&& right)
{
    if (left.is_empty()) {
        target.extend(right);
        return;
    }

    if (right.is_empty()) {
        target.extend(left);
        return;
    }

    size_t left_skip = 0;
    MatchState state;
    for (state.instruction_position = 0; state.instruction_position < left.size() && state.instruction_position < right.size();) {
        auto left_size = left.get_opcode(state).size();
        auto right_size = right.get_opcode(state).size();
        if (left_size != right_size)
            break;

        if (left.spans().slice(state.instruction_position, left_size) == right.spans().slice(state.instruction_position, right_size))
            left_skip = state.instruction_position + left_size;
        else
            break;

        state.instruction_position += left_size;
    }

    dbgln_if(REGEX_DEBUG, "Skipping {}/{} bytecode entries from {}/{}", left_skip, 0, left.size(), right.size());

    if (left_skip) {
        target.extend(left.release_slice(0, left_skip));
        right = right.release_slice(left_skip);
    }

    auto left_size = left.size();

    target.empend(static_cast<ByteCodeValueType>(OpCodeId::ForkJump));
    target.empend(right.size() + (left_size ? 2 : 0)); // Jump to the _ALT label

    target.extend(move(right));

    if (left_size != 0) {
        target.empend(static_cast<ByteCodeValueType>(OpCodeId::Jump));
        target.empend(left.size()); // Jump to the _END label
    }

    // LABEL _ALT = bytecode.size() + 2

    target.extend(move(left));

    // LABEL _END = alterantive_bytecode.size
}

template void Regex<PosixBasicParser>::run_optimization_passes();
template void Regex<PosixExtendedParser>::run_optimization_passes();
template void Regex<ECMA262Parser>::run_optimization_passes();
}
