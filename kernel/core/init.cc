﻿#include <ec/util.h>
#include <libc/str.h>

#include "../../boot/boot.h"
#include "../hw/acpi/acpi.h"
#include "../hw/cmos/cmos.h"
#include "../hw/cpu/isr.h"
#include "../hw/cpu/x64.h"
#include "../hw/ps2/ps2.h"
#include "../hw/serial/serial.h"
#include "../hw/timer/timer.h"
#include "gfx/output.h"

#include "../common/mm.h"

static constexpr size_t kernel_stack_size = KiB(8);
alignas(page_size) volatile u8 kernel_stack[kernel_stack_size];
EXTERN_C u64 kernel_stack_top = ( u64 )&kernel_stack[kernel_stack_size];

EARLY static void IterateMemoryDescriptors(const MemoryMap& m, auto&& callback)
{
    for (auto desc = m.map;
        desc < uefi_next_memory_descriptor(m.map, m.size);
        desc = uefi_next_memory_descriptor(desc, m.descriptor_size))
    {
        callback(desc);
    }
}

EARLY static void SerialPrintDescriptors(MemoryMap& m)
{
    i32 i = 0;
    serial::Write("==== MEMORY MAP ====\n");
    IterateMemoryDescriptors(m, [&i](uefi::memory_descriptor* desc)
    {
        serial::Write(
            "[%d]: Type: %d   PA: 0x%llx   VA: 0x%llx (pages: %llu) Attr 0x%llx\n",
            i++,
            desc->type,
            desc->physical_start,
            desc->virtual_start,
            desc->number_of_pages,
            desc->attribute
        );
    });
    serial::Write("====================\n");
}

static IMAGE_NT_HEADERS* ImageNtHeaders(void* image_base)
{
    auto dos_header = ( IMAGE_DOS_HEADER* )image_base;
    if (!dos_header || dos_header->e_magic != IMAGE_DOS_SIGNATURE)
        return nullptr;

    auto nt_headers = ( IMAGE_NT_HEADERS* )(( uintptr_t )image_base + dos_header->e_lfanew);
    if (!nt_headers || nt_headers->Signature != IMAGE_NT_SIGNATURE)
        return nullptr;

    return nt_headers;
}

EXTERN_C NO_RETURN void OsInitialize(LoaderBlock* loader_block)
{
    gfx::Initialize(loader_block->display);

    // Init COM ports so we have early debugging capabilities.
    // TODO - Remap APIC earlier when we use serial interrupts
    serial::Initialize();

    acpi::ParseMadt(loader_block->madt_header, x64::cpu_info);

    // Copy all the important stuff because the loader block gets freed in ReclaimBootPages
    MemoryMap memory_map = loader_block->memory_map;
    DisplayInfo display = loader_block->display;
    KernelData kernel = loader_block->kernel;
    auto hpet = loader_block->hpet;
    auto i8042 = loader_block->i8042;
    const auto pt_physical = loader_block->page_tables_pool;
    const auto pt_pages = loader_block->page_tables_pool_count;

    // Build a new page table (the bootloader one is temporary)
    mm::PagePool pool(kva::kernel_pt.base, pt_physical, pt_pages);
    mm::AllocatePhysical(pool, &pool.root);

    const auto kernel_pages = SizeToPages(kernel.size);
    const auto frame_buffer_pages = SizeToPages(display.frame_buffer_size);

    mm::MapPages(pool, kva::kernel.base, kernel.physical_base, kernel_pages);
    mm::MapPages(pool, kva::kernel_pt.base, pt_physical, pt_pages);

    // turns out vbox page faults at fb base + 0x3000000 when we reach the end so just map the whole range
    mm::MapPagesInRegion(pool, kva::frame_buffer, &display.frame_buffer, kva::frame_buffer.PageCount());
    mm::MapPagesInRegion(pool, kva::devices, &hpet, 1);
    mm::MapPagesInRegion(pool, kva::devices, &apic::io, 1);
    mm::MapPagesInRegion(pool, kva::devices, &apic::local, 1);

    // SerialPrintDescriptors(memory_map);

    IterateMemoryDescriptors(memory_map, [&pool](uefi::memory_descriptor* desc)
    {
        if (desc->attribute & uefi::memory_runtime)
            MapPages(pool, desc->virtual_start, desc->physical_start, desc->number_of_pages);
    });

    __writecr3(pool.root);

    gfx::SetFrameBufferAddress(display.frame_buffer); // TODO - set this earlier?

    x64::cpu_info.using_apic = false;
    x64::Initialize();

    timer::Initialize(hpet);

    if (i8042)
        ps2::Initialize();
    else
        Print("No PS/2 legacy support.\n");

    // while (1)
    // {
    //     volatile u8 last = time.s;
    //     u64 x = __rdtsc();
    //     volatile u64 next = timer::ticks + 1000;
    //     while (next > timer::ticks)
    //         ;
    //     Print("%llu (%llu) ", ++a, __rdtsc() - x);
    //     while (last == time.s)
    //         ;
    // }

    // Now that kernel init has completed, zero out discardable sections (INIT, CRT, RELOC)
    // and write-protect everything without the IMAGE_SCN_MEM_WRITE attribute.

    auto kernel_nt = ImageNtHeaders(( void* )kva::kernel.base);
    auto section = IMAGE_FIRST_SECTION(kernel_nt);

    for (u16 i = 0; i < kernel_nt->FileHeader.NumberOfSections; i++)
    {
        char section_name[IMAGE_SIZEOF_SHORT_NAME + 1]{};
        strlcpy(section_name, ( const char* )section->Name, IMAGE_SIZEOF_SHORT_NAME);
        if (section->Characteristics & IMAGE_SCN_MEM_DISCARDABLE)
        {
            const auto start = kva::kernel.base + section->VirtualAddress;
            const auto size = section->Misc.VirtualSize;
            Print(
                "Zeroing section %s at 0x%llx (%u bytes)\n",
                section_name,
                start,
                size
            );
            memzero(( void* )start, size);
        }
        else if (!(section->Characteristics & IMAGE_SCN_MEM_WRITE))
        {
            const auto start = kva::kernel.base + section->VirtualAddress;
            const auto end = start + section->Misc.VirtualSize;
            Print(
                "Write-protecting section %s at 0x%llx (%llu pages)\n",
                section_name,
                start,
                end
            );
            for (auto page = start; page < end; page += page_size)
            {
                auto pte = mm::GetPresentPte(pool, page);
                pte->writable = false;
                x64::TlbFlushAddress(( void* )page);
            }
        }
        section++;
    }

    x64::unmask_interrupts();
    x64::Idle();
}
