// SPDX-License-Identifier: GPL-2.0-or-later
// tlg: alpha-tested hair and grass discard against interleaved gradient noise,
// fract(52.98 * fract(0.0671 * x + 0.0058 * y)) on FragCoord. The noise is built for steps of one
// pixel. In a reduced pass FragCoord is converted to native pixels (a step of 1.5 or 2), and the
// noise turns into a slow ramp: stripes at DLSS Quality, whole see-through patches at
// Performance. Such a noise input reads FragCoord with index 1 instead: the pixel grid the pass
// renders at, unconverted (EmitGetAttribute).

#include <cstdio>
#include <unordered_map>
#include "shader_recompiler/ir/basic_block.h"
#include "shader_recompiler/ir/passes/ir_passes.h"
#include "shader_recompiler/ir/program.h"

namespace Shader::Optimization {

namespace {
bool IsFragCoordXY(const IR::Value& value) {
    const IR::Inst* inst = value.IsImmediate() ? nullptr : value.TryInst();
    return inst && inst->GetOpcode() == IR::Opcode::GetAttribute &&
           inst->Arg(0).Attribute() == IR::Attribute::FragCoord && inst->Arg(1).U32() < 2 &&
           inst->Arg(2).U32() == 0;
}

/// Counts the FragCoord operands of the arithmetic feeding a fract.
void FindNoiseInputs(IR::Inst& inst, int depth, std::unordered_map<IR::Inst*, int>& uses) {
    switch (inst.GetOpcode()) {
    case IR::Opcode::FPAdd32:
    case IR::Opcode::FPSub32:
    case IR::Opcode::FPMul32:
    case IR::Opcode::FPFma32:
        break;
    default:
        return;
    }
    for (size_t i = 0; i < inst.NumArgs(); ++i) {
        const IR::Value arg = inst.Arg(i);
        if (IsFragCoordXY(arg)) {
            ++uses[arg.TryInst()];
        } else if (depth > 0 && !arg.IsImmediate()) {
            if (IR::Inst* producer = arg.TryInst()) {
                FindNoiseInputs(*producer, depth - 1, uses);
            }
        }
    }
}
} // namespace

void FragCoordNoisePass(IR::Program& program) {
    std::unordered_map<IR::Inst*, int> uses;
    for (IR::Block* const block : program.blocks) {
        for (IR::Inst& inst : block->Instructions()) {
            if (inst.GetOpcode() == IR::Opcode::FPFract32 && !inst.Arg(0).IsImmediate()) {
                if (IR::Inst* input = inst.Arg(0).TryInst()) {
                    FindNoiseInputs(*input, 2, uses);
                }
            }
        }
    }
    // A FragCoord that also addresses textures keeps the converted coordinates.
    u32 patched = 0, shared = 0;
    for (const auto& [coord, count] : uses) {
        if (count == coord->UseCount()) {
            coord->SetArg(2, IR::Value(1u));
            ++patched;
        } else {
            ++shared;
        }
    }
    if (patched || shared) {
        static u32 logged = 0;
        if (logged++ < 64) {
            std::printf("Shader: FragCoord noise on the pass grid, %u inputs (%u shared) in %016llx\n",
                        patched, shared, static_cast<unsigned long long>(program.info.pgm_hash));
        }
    }
}

} // namespace Shader::Optimization
