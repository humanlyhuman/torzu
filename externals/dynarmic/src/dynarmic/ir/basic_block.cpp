/* This file is part of the dynarmic project.
 * Copyright (c) 2016 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#include "dynarmic/ir/basic_block.h"

#include <algorithm>
#include <initializer_list>
#include <map>
#include <string>

#include <fmt/format.h>
#include <mcl/assert.hpp>

#include "dynarmic/common/memory_pool.h"
#include "dynarmic/frontend/A32/a32_types.h"
#include "dynarmic/frontend/A64/a64_types.h"
#include "dynarmic/ir/cond.h"
#include "dynarmic/ir/opcodes.h"

namespace Dynarmic::IR {

Block::Block(const LocationDescriptor& location)
        : location{location}, end_location{location}, cond{Cond::AL}, instruction_alloc_pool{std::make_unique<Common::Pool>(sizeof(Inst), 4096)} {}

Block::~Block() = default;

Block::Block(Block&&) = default;

Block& Block::operator=(Block&&) = default;

Block::iterator Block::AppendNewInst(Opcode opcode, std::initializer_list<IR::Value> args) {
    return PrependNewInst(end(), opcode, args);
}

Block::iterator Block::AppendNewInst(Inst&& inst) {
    return PrependNewInst(end(), std::move(inst));
}

Block::iterator Block::PrependNewInst(iterator insertion_point, Opcode opcode, std::initializer_list<Value> args) {
    IR::Inst* inst = new (instruction_alloc_pool->Alloc()) IR::Inst(opcode);
    ASSERT(args.size() == inst->NumArgs());

    std::for_each(args.begin(), args.end(), [&inst, index = size_t(0)](const auto& arg) mutable {
        inst->SetArg(index, arg);
        index++;
    });

    return instructions.insert_before(insertion_point, inst);
}

Block::iterator Block::PrependNewInst(iterator insertion_point, Inst&& inst) {
    IR::Inst* new_inst = new (instruction_alloc_pool->Alloc()) IR::Inst(std::move(inst));

    return instructions.insert_before(insertion_point, new_inst);
}

LocationDescriptor Block::Location() const {
    return location;
}

LocationDescriptor Block::EndLocation() const {
    return end_location;
}

void Block::SetEndLocation(const LocationDescriptor& descriptor) {
    end_location = descriptor;
}

Cond Block::GetCondition() const {
    return cond;
}

void Block::SetCondition(Cond condition) {
    cond = condition;
}

LocationDescriptor Block::ConditionFailedLocation() const {
    return *cond_failed;
}

void Block::SetConditionFailedLocation(LocationDescriptor fail_location) {
    cond_failed = fail_location;
}

size_t& Block::ConditionFailedCycleCount() {
    return cond_failed_cycle_count;
}

const size_t& Block::ConditionFailedCycleCount() const {
    return cond_failed_cycle_count;
}

bool Block::HasConditionFailedLocation() const {
    return cond_failed.has_value();
}

Block::InstructionList& Block::Instructions() {
    return instructions;
}

const Block::InstructionList& Block::Instructions() const {
    return instructions;
}

Terminal Block::GetTerminal() const {
    return terminal;
}

void Block::SetTerminal(Terminal term) {
    ASSERT_MSG(!HasTerminal(), "Terminal has already been set.");
    terminal = std::move(term);
}

void Block::ReplaceTerminal(Terminal term) {
    ASSERT_MSG(HasTerminal(), "Terminal has not been set.");
    terminal = std::move(term);
}

bool Block::HasTerminal() const {
    return terminal.which() != 0;
}

size_t& Block::CycleCount() {
    return cycle_count;
}

const size_t& Block::CycleCount() const {
    return cycle_count;
}

void Block::Serialize(std::vector<uint16_t>& fres) const {
    ASSERT(!empty());

    fres.push_back(0xa91e);

    fres.push_back(size());
    for (const auto& inst : *this)
        inst.Serialize(*this, fres);

    SerializeTerminal(GetTerminal(), fres);
    EndLocation().Serialize(fres);
}

void Block::Deserialize(std::vector<uint16_t>::iterator& it) {
    ASSERT(empty());

    const bool magic_ok = *(it++) == 0xa91e;
    ASSERT_MSG(magic_ok, "Bad IR block magic");

    const auto inst_count = *(it++);
    ASSERT(inst_count > 0);
    std::vector<Inst*> insts;
    for (unsigned idx = 0; idx != inst_count; ++idx) {
        auto inst = Inst::Deserialize(insts, it);
        Inst* ptr = &*AppendNewInst(std::move(inst));
        insts.push_back(ptr);
    }

    SetTerminal(DeserializeTerminal(it));
    SetEndLocation(LocationDescriptor::Deserialize(it));
}

void Block::SerializeTerminal(const Term::Terminal& term, std::vector<uint16_t>& fres) {
    fres.push_back(0xa91f);

    struct Visitor : boost::static_visitor<void> {
        std::vector<uint16_t>& fres;

        Visitor(std::vector<uint16_t>& fres) : fres(fres) {}

        void operator()(const Term::Invalid&) const {
            fres.push_back(0);
        }
        void operator()(const Term::Interpret& interp) const {
            fres.push_back(1);
            interp.next.Serialize(fres);
            ASSERT(interp.num_instructions <= 0xffff);
            fres.push_back(static_cast<uint16_t>(interp.num_instructions));
        }
        void operator()(const Term::ReturnToDispatch&) const {
            fres.push_back(2);
        }
        void operator()(const Term::LinkBlock& link_block) const {
            fres.push_back(3);
            link_block.next.Serialize(fres);
        }
        void operator()(const Term::LinkBlockFast& link_block_fast) const {
            fres.push_back(4);
            link_block_fast.next.Serialize(fres);
        }
        void operator()(const Term::PopRSBHint&) const {
            fres.push_back(5);
        }
        void operator()(const Term::FastDispatchHint&) const {
            fres.push_back(6);
        }
        void operator()(const Term::If& if_) const {
            fres.push_back(7);
            fres.push_back(static_cast<uint16_t>(if_.if_));
            SerializeTerminal(if_.then_, fres);
            SerializeTerminal(if_.else_, fres);
        }
        void operator()(const Term::CheckBit& check_bit) const {
            fres.push_back(8);
            SerializeTerminal(check_bit.then_, fres);
            SerializeTerminal(check_bit.else_, fres);
        }
        void operator()(const Term::CheckHalt& check_bit) const {
            fres.push_back(9);
            SerializeTerminal(check_bit.else_, fres);
        }
    } visitor{fres};

    boost::apply_visitor(visitor, term);
}

Term::Terminal Block::DeserializeTerminal(std::vector<uint16_t>::iterator& it) {
    const bool magic_ok = *(it++) == 0xa91f;
    ASSERT_MSG(magic_ok, "Bad IR block magic");

    Term::Terminal fres;

    const auto term_idx = *(it++);
    switch (term_idx) {
    case 0: {
        fres = Term::Invalid();
    } break;
    case 1: {
        Term::Interpret interp(LocationDescriptor::Deserialize(it));
        interp.num_instructions = *(it++);
        fres = std::move(interp);
    } break;
    case 2: {
        fres = Term::ReturnToDispatch();
    } break;
    case 3: {
        fres = Term::LinkBlock(LocationDescriptor::Deserialize(it));
    } break;
    case 4: {
        fres = Term::LinkBlockFast(LocationDescriptor::Deserialize(it));
    } break;
    case 5: {
        fres = Term::PopRSBHint();
    } break;
    case 6: {
        fres = Term::FastDispatchHint();
    } break;
    case 7: {
        const auto cond = static_cast<Cond>(*(it++));
        Term::Terminal then = DeserializeTerminal(it);
        Term::Terminal else_ = DeserializeTerminal(it);
        fres = Term::If(cond, std::move(then), std::move(else_));
    } break;
    case 8: {
        Term::Terminal then = DeserializeTerminal(it);
        Term::Terminal else_ = DeserializeTerminal(it);
        fres = Term::CheckBit(std::move(then), std::move(else_));
    } break;
    case 9: {
        Term::Terminal else_ = DeserializeTerminal(it);
        fres = Term::CheckHalt(std::move(else_));
    } break;
    default: ASSERT_FALSE("Invalid terminal type index");
    }

    return fres;
}

static std::string TerminalToString(const Terminal& terminal_variant) {
    struct : boost::static_visitor<std::string> {
        std::string operator()(const Term::Invalid&) const {
            return "<invalid terminal>";
        }
        std::string operator()(const Term::Interpret& terminal) const {
            return fmt::format("Interpret{{{}}}", terminal.next);
        }
        std::string operator()(const Term::ReturnToDispatch&) const {
            return "ReturnToDispatch{}";
        }
        std::string operator()(const Term::LinkBlock& terminal) const {
            return fmt::format("LinkBlock{{{}}}", terminal.next);
        }
        std::string operator()(const Term::LinkBlockFast& terminal) const {
            return fmt::format("LinkBlockFast{{{}}}", terminal.next);
        }
        std::string operator()(const Term::PopRSBHint&) const {
            return "PopRSBHint{}";
        }
        std::string operator()(const Term::FastDispatchHint&) const {
            return "FastDispatchHint{}";
        }
        std::string operator()(const Term::If& terminal) const {
            return fmt::format("If{{{}, {}, {}}}", A64::CondToString(terminal.if_), TerminalToString(terminal.then_), TerminalToString(terminal.else_));
        }
        std::string operator()(const Term::CheckBit& terminal) const {
            return fmt::format("CheckBit{{{}, {}}}", TerminalToString(terminal.then_), TerminalToString(terminal.else_));
        }
        std::string operator()(const Term::CheckHalt& terminal) const {
            return fmt::format("CheckHalt{{{}}}", TerminalToString(terminal.else_));
        }
    } visitor;

    return boost::apply_visitor(visitor, terminal_variant);
}

std::string DumpBlock(const IR::Block& block) {
    std::string ret;

    ret += fmt::format("Block: location={}\n", block.Location());
    ret += fmt::format("cycles={}", block.CycleCount());
    ret += fmt::format(", entry_cond={}", A64::CondToString(block.GetCondition()));
    if (block.GetCondition() != Cond::AL) {
        ret += fmt::format(", cond_fail={}", block.ConditionFailedLocation());
    }
    ret += '\n';

    const auto arg_to_string = [](const IR::Value& arg) -> std::string {
        if (arg.IsEmpty()) {
            return "<null>";
        } else if (!arg.IsImmediate()) {
            if (const unsigned name = arg.GetInst()->GetName()) {
                return fmt::format("%{}", name);
            }
            return fmt::format("%<unnamed inst {:016x}>", reinterpret_cast<u64>(arg.GetInst()));
        }
        switch (arg.GetType()) {
        case Type::U1:
            return fmt::format("#{}", arg.GetU1() ? '1' : '0');
        case Type::U8:
            return fmt::format("#{}", arg.GetU8());
        case Type::U16:
            return fmt::format("#{:#x}", arg.GetU16());
        case Type::U32:
            return fmt::format("#{:#x}", arg.GetU32());
        case Type::U64:
            return fmt::format("#{:#x}", arg.GetU64());
        case Type::A32Reg:
            return A32::RegToString(arg.GetA32RegRef());
        case Type::A32ExtReg:
            return A32::ExtRegToString(arg.GetA32ExtRegRef());
        case Type::A64Reg:
            return A64::RegToString(arg.GetA64RegRef());
        case Type::A64Vec:
            return A64::VecToString(arg.GetA64VecRef());
        default:
            return "<unknown immediate type>";
        }
    };

    for (const auto& inst : block) {
        const Opcode op = inst.GetOpcode();

        ret += fmt::format("[{:016x}] ", reinterpret_cast<u64>(&inst));
        if (GetTypeOf(op) != Type::Void) {
            if (inst.GetName()) {
                ret += fmt::format("%{:<5} = ", inst.GetName());
            } else {
                ret += "noname = ";
            }
        } else {
            ret += "         ";  // '%00000 = ' -> 1 + 5 + 3 = 9 spaces
        }

        ret += GetNameOf(op);

        const size_t arg_count = GetNumArgsOf(op);
        for (size_t arg_index = 0; arg_index < arg_count; arg_index++) {
            const Value arg = inst.GetArg(arg_index);

            ret += arg_index != 0 ? ", " : " ";
            ret += arg_to_string(arg);

            Type actual_type = arg.GetType();
            Type expected_type = GetArgTypeOf(op, arg_index);
            if (!AreTypesCompatible(actual_type, expected_type)) {
                ret += fmt::format("<type error: {} != {}>", GetNameOf(actual_type), GetNameOf(expected_type));
            }
        }

        ret += fmt::format(" (uses: {})", inst.UseCount());

        ret += '\n';
    }

    ret += "terminal = " + TerminalToString(block.GetTerminal()) + '\n';

    return ret;
}

}  // namespace Dynarmic::IR
