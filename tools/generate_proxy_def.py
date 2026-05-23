#!/usr/bin/env python3
import argparse
import struct
from pathlib import Path

INTERCEPTED = {
    "mpv_command",
    "mpv_command_async",
    "mpv_command_string",
    "mpv_create",
    "mpv_create_client",
    "mpv_create_weak_client",
    "mpv_destroy",
    "mpv_free",
    "mpv_get_property",
    "mpv_get_property_string",
    "mpv_initialize",
    "mpv_set_property",
    "mpv_set_property_string",
    "mpv_terminate_destroy",
    "mpv_wait_event",
    "mpv_wakeup",
}


def u16(data, offset):
    return struct.unpack_from("<H", data, offset)[0]


def u32(data, offset):
    return struct.unpack_from("<I", data, offset)[0]


def parse_exports(path):
    data = path.read_bytes()
    pe = u32(data, 0x3C)
    section_count = u16(data, pe + 6)
    optional_header = pe + 24
    magic = u16(data, optional_header)
    data_directory = optional_header + (112 if magic == 0x20B else 96)
    section_table = optional_header + u16(data, pe + 20)

    sections = []
    for index in range(section_count):
        offset = section_table + index * 40
        virtual_size = u32(data, offset + 8)
        virtual_address = u32(data, offset + 12)
        raw_size = u32(data, offset + 16)
        raw_pointer = u32(data, offset + 20)
        sections.append((virtual_address, max(virtual_size, raw_size), raw_pointer))

    def rva_to_offset(rva):
        for virtual_address, size, raw_pointer in sections:
            if virtual_address <= rva < virtual_address + size:
                return raw_pointer + (rva - virtual_address)
        raise ValueError(f"RVA 0x{rva:x} 不在任一节内")

    def c_string(offset):
        end = data.find(b"\0", offset)
        return data[offset:end].decode("utf-8", errors="replace")

    export_rva = u32(data, data_directory)
    export_offset = rva_to_offset(export_rva)
    ordinal_base = u32(data, export_offset + 16)
    function_count = u32(data, export_offset + 20)
    name_count = u32(data, export_offset + 24)
    names_offset = rva_to_offset(u32(data, export_offset + 32))
    ordinals_offset = rva_to_offset(u32(data, export_offset + 36))

    names_by_ordinal = {}
    for index in range(name_count):
        name_offset = rva_to_offset(u32(data, names_offset + index * 4))
        ordinal_index = u16(data, ordinals_offset + index * 2)
        names_by_ordinal[ordinal_base + ordinal_index] = c_string(name_offset)

    exports = []
    for index in range(function_count):
        ordinal = ordinal_base + index
        name = names_by_ordinal.get(ordinal)
        if not name:
            raise ValueError(f"发现无名称导出 ordinal={ordinal}，当前代理不支持")
        exports.append((ordinal, name))
    return exports


def sanitize_symbol(name):
    result = []
    for ch in name:
        if ch.isalnum() or ch == "_":
            result.append(ch)
        else:
            result.append("_")
    return "p_" + "".join(result)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--asm-output", required=True, type=Path)
    parser.add_argument("--table-output", required=True, type=Path)
    args = parser.parse_args()

    exports = parse_exports(args.input)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.asm_output.parent.mkdir(parents=True, exist_ok=True)
    args.table_output.parent.mkdir(parents=True, exist_ok=True)

    lines = ["LIBRARY libmpv-2", "EXPORTS"]
    for ordinal, name in exports:
        lines.append(f"    {name} @{ordinal}")

    args.output.write_text("\n".join(lines) + "\n", encoding="utf-8")

    forwarded = [(ordinal, name) for ordinal, name in exports if name not in INTERCEPTED]

    asm_lines = ["OPTION CASEMAP:NONE", ""]
    for _, name in forwarded:
        asm_lines.append(f"EXTERN {sanitize_symbol(name)}:QWORD")
    asm_lines.append("")
    asm_lines.append(".code")
    for _, name in forwarded:
        asm_lines.append(f"{name} PROC")
        asm_lines.append(f"    jmp QWORD PTR [{sanitize_symbol(name)}]")
        asm_lines.append(f"{name} ENDP")
    asm_lines.append("END")
    args.asm_output.write_text("\n".join(asm_lines) + "\n", encoding="ascii")

    table_lines = [
        "#include <windows.h>",
        "",
        "extern \"C\" void MissingForwardedExport();",
    ]
    for _, name in forwarded:
        table_lines.append(f"extern \"C\" void* {sanitize_symbol(name)} = reinterpret_cast<void*>(&MissingForwardedExport);")
    table_lines.extend(
        [
            "",
            "struct ForwardedExportEntry {",
            "    const char* name;",
            "    void** slot;",
            "};",
            "",
            "static ForwardedExportEntry kForwardedExports[] = {",
        ]
    )
    for _, name in forwarded:
        table_lines.append(f"    {{\"{name}\", &{sanitize_symbol(name)}}},")
    table_lines.extend(
        [
            "};",
            "",
            "extern \"C\" void InitializeForwardedExports(HMODULE real_module) {",
            "    for (auto& entry : kForwardedExports) {",
            "        FARPROC proc = real_module ? GetProcAddress(real_module, entry.name) : nullptr;",
            "        *entry.slot = reinterpret_cast<void*>(proc ? proc : reinterpret_cast<FARPROC>(&MissingForwardedExport));",
            "    }",
            "}",
        ]
    )
    args.table_output.write_text("\n".join(table_lines) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
