# Generate a corrected copy instead of modifying the vendored QPM dependency.
# Fail closed when the dependency changes: review a new version before using it.
function(vivify_fix_arm64_relocator input output)
  file(SHA256 "${input}" source_sha)
  if(NOT source_sha STREQUAL "bf37f249ff86b56a20330a8a92c530a83882359dcfe5359e8d71a56a7acc5683")
    message(FATAL_ERROR "Unreviewed And64InlineHook.cpp (${source_sha}); re-audit the signed ARM64 relocation fix")
  endif()
  file(READ "${input}" source)
  string(REPLACE "#include \"../../shared/inline-hook/And64InlineHook.hpp\""
    "#include \"And64InlineHook.hpp\"\n#include \"VivifyArm64Relocation.hpp\"\n#include \"VivifyArm64HookStub.hpp\"" source "${source}")
  string(REPLACE "((ins & ~lmask) >> (lsb - 2u))"
    "(lmask == lmask2 ? Vivify::Arm64Relocation::TestBranchDisplacement(ins) : Vivify::Arm64Relocation::ConditionalBranchDisplacement(ins))" source "${source}")
  string(REPLACE "((static_cast<int32_t>(ins << msb) >> (msb + lsb - 2u)) & ~3u)"
    "Vivify::Arm64Relocation::LiteralDisplacement(ins)" source "${source}")
  # ADR has a two-bit byte offset; ADRP shifts the entire signed imm21 by 12.
  string(REPLACE "(Vivify::Arm64Relocation::LiteralDisplacement(ins) | lsb_bytes)"
    "Vivify::Arm64Relocation::AdrDisplacement(ins)" source "${source}")
  string(REPLACE "(Vivify::Arm64Relocation::AdrDisplacement(ins) << 12)"
    "Vivify::Arm64Relocation::AdrpDisplacement(ins)" source "${source}")
  # A page address stays the original page even when the prologue starts on
  # that page boundary. Re-emitting ADRP at the trampoline changes its value.
  set(old_adrp_case [=[if (ctxp->is_in_fixing_range(absolute_addr)) {
                intptr_t ref_idx = ctxp->get_ref_ins_index(absolute_addr/* & ~3ull*/);
                if (ref_idx > current_idx) {
                    // the bottom 12 bits of absolute_addr are masked out,
                    // so ref_idx must be less than or equal to current_idx!
                    A64_LOGE("ref_idx must be less than or equal to current_idx!");
                } //if

                // *absolute_addr may be changed due to relocation fixing
                A64_LOGI("What is the correct way to fix this?");
                *(*outpp)++ = ins; // 0x90000000u;
            } else {]=])
  string(FIND "${source}" "${old_adrp_case}" adrp_case_position)
  if(adrp_case_position EQUAL -1)
    message(FATAL_ERROR "Expected original-page ADRP case is missing")
  endif()
  string(REPLACE "${old_adrp_case}" "{ // Preserve the original page address, including prologue pages." source "${source}")
  # Encoding a negative relocated displacement must use an unsigned left shift.
  string(REPLACE "static_cast<uint32_t>(new_pc_offset << lsb)"
    "(static_cast<uint32_t>(new_pc_offset) << lsb)" source "${source}")
  string(REPLACE "static_cast<uint32_t>(new_pc_offset << (lsb - 2u))"
    "(static_cast<uint32_t>(new_pc_offset) << (lsb - 2u))" source "${source}")
  string(REPLACE "((int32_t(this->dat[idx].ins - reinterpret_cast<int64_t>(f.bp)) >> 2) << f.ls)"
    "(static_cast<uint32_t>(int32_t(this->dat[idx].ins - reinterpret_cast<int64_t>(f.bp)) >> 2) << f.ls)" source "${source}")
  # Existing hook entries contain pointer DATA after LDR/BR. Never run the
  # instruction decoder over that pool. Preserve the previous target through
  # a code-only trampoline, fully populated before publishing the new entry.
  # New entries contain only MOVZ/MOVK/MOVK/BR (+NOP), which older installers
  # can relocate without mistaking a replacement pointer for instructions.
  set(old_install [=[        uint32_t *trampoline = static_cast<uint32_t *>(rwx), *original = static_cast<uint32_t *>(symbol);

        static_assert(A64_MAX_INSTRUCTIONS >= 5, "please fix A64_MAX_INSTRUCTIONS!");
        int32_t count = (reinterpret_cast<uint64_t>(original + 2) & 7u) != 0u ? 5 : 4;
        if (trampoline) {
            if (rwx_size < count * 10u) {
                //  LOGW("rwx size is too small to hold %u bytes backup instructions!", count * 10u);
                return NULL;
            } //if
            __fix_instructions(original, count, trampoline);
        } //if

        if (__make_rwx(original, 5 * sizeof(uint32_t)) == 0) {
            if (count == 5) {
                original[0] = A64_NOP;
                ++original;
            } //if
            original[0] = 0x58000051u; // LDR X17, #0x8
            original[1] = 0xd61f0220u; // BR X17
            *reinterpret_cast<int64_t *>(original + 2) = __intval(replace);
            __flush_cache(symbol, 5 * sizeof(uint32_t));

            A64_LOGI("inline hook %p->%p successfully! %zu bytes overwritten",
                        symbol, replace, 5 * sizeof(uint32_t));
        } else {
            A64_LOGE("mprotect failed with errno = %d, p = %p, size = %zu",
                        errno, original, 5 * sizeof(uint32_t));
            trampoline = NULL;
        } //if

        return trampoline;]=])
  set(new_install [=[        namespace Stub = Vivify::Arm64HookStub;
        auto *original = static_cast<uint32_t *>(symbol);
        auto *trampoline = static_cast<uint32_t *>(rwx);
        auto const address = reinterpret_cast<uint64_t>(symbol);
        auto const replacement = reinterpret_cast<uint64_t>(replace);
        auto const count = Stub::EntryWordCount(address);
        auto const emitted = Stub::Encode48(replacement);
        if (!count || !emitted || address == replacement) {
            A64_LOGE("Vivify hook: unsupported or self-referential target %p -> %p", symbol, replace);
            return NULL;
        }
        std::array<uint32_t, 5> snapshot{};
        memcpy(snapshot.data(), original, count * sizeof(uint32_t));
        auto const words = std::span<uint32_t const>(snapshot.data(), count);
        if (Stub::HasStubPrefix(words, address)) {
            auto const existing = Stub::DecodeExisting(words, address, replacement);
            if (!existing) {
                A64_LOGE("Vivify hook: refused malformed or recursive existing stub at %p", symbol);
                return NULL;
            }
            if (trampoline) {
                auto const prior = Stub::Encode48(existing->target);
                auto const priorCount = Stub::EntryWordCount(reinterpret_cast<uint64_t>(trampoline));
                if (!prior || !priorCount || rwx_size < priorCount * sizeof(uint32_t)) {
                    A64_LOGE("Vivify hook: existing-chain trampoline is unsupported or too small");
                    return NULL;
                }
                memcpy(trampoline, prior->data(), sizeof(Stub::Words));
                if (priorCount == 5) trampoline[4] = Stub::Nop;
                __flush_cache(trampoline, priorCount * sizeof(uint32_t));
            }
            __android_log_print(ANDROID_LOG_INFO, "A64_HOOK",
                "Vivify safe chain: entry=%p prior=%p replacement=%p kind=%u words=%08x,%08x,%08x,%08x,%08x",
                symbol, reinterpret_cast<void *>(existing->target), replace, static_cast<unsigned>(existing->kind),
                snapshot[0], snapshot[1], snapshot[2], snapshot[3], snapshot[4]);
        } else if (trampoline) {
            if (rwx_size < count * 10u * sizeof(uint32_t)) {
                A64_LOGE("Vivify hook: trampoline buffer is too small");
                return NULL;
            }
            __fix_instructions(original, static_cast<int32_t>(count), trampoline);
        }
        auto const size = count * sizeof(uint32_t);
        if (__make_rwx(original, size) != 0) {
            A64_LOGE("mprotect failed with errno = %d, p = %p, size = %zu", errno, original, size);
            return NULL;
        }
        memcpy(original, emitted->data(), sizeof(Stub::Words));
        if (count == 5) original[4] = Stub::Nop;
        __flush_cache(symbol, size);
        return trampoline;]=])
  string(FIND "${source}" "${old_install}" install_position)
  if(install_position EQUAL -1)
    message(FATAL_ERROR "Expected original hook installation body is missing")
  endif()
  string(REPLACE "${old_install}" "${new_install}" source "${source}")
  string(REPLACE "trampoline = A64HookFunctionV(symbol, replace, trampoline, A64_MAX_INSTRUCTIONS * 10u);"
    "trampoline = A64HookFunctionV(symbol, replace, trampoline, sizeof(__insns_pool[0]));" source "${source}")
  string(REPLACE "if (trampoline == NULL && result != NULL) {\n            *result = NULL;"
    "if (result != NULL) {\n            *result = trampoline;" source "${source}")
  get_filename_component(output_dir "${output}" DIRECTORY)
  file(MAKE_DIRECTORY "${output_dir}")
  # configure_file avoids recompilation when configuration produces identical bytes.
  file(WRITE "${output}.in" "${source}")
  configure_file("${output}.in" "${output}" COPYONLY)
endfunction()
