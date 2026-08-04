/*
 * Partial-link script for .tapp output.
 *
 * clang always emits .ARM.exidx/.ARM.extab unwind tables (one pair per function) and their
 * .rel sections carry R_ARM_PREL31 (type 42). The firmware ELF loader rejects any relocation
 * type it doesn't implement (lib/tapp/elf/elf_file.c, apply_relocation) so a tapp carrying
 * them fails to load with "Unsupported relocation type".
 *
 * -fno-unwind-tables / -fno-asynchronous-unwind-tables do NOT suppress them on ARM, and there
 * is no codegen flag that does. Dropping them at partial-link time is the fix; the resulting
 * relocation set (ABS32 / THM_CALL / THM_JUMP24) matches what arm-none-eabi-gcc produced.
 */
SECTIONS { /DISCARD/ : { *(.ARM.exidx*) *(.ARM.extab*) } }
