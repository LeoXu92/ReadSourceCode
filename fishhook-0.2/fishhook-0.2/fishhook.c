// Copyright (c) 2013, Facebook, Inc.
// All rights reserved.
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//   * Redistributions of source code must retain the above copyright notice,
//     this list of conditions and the following disclaimer.
//   * Redistributions in binary form must reproduce the above copyright notice,
//     this list of conditions and the following disclaimer in the documentation
//     and/or other materials provided with the distribution.
//   * Neither the name Facebook nor the names of its contributors may be used to
//     endorse or promote products derived from this software without specific
//     prior written permission.
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
// FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
// DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
// SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
// CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
// OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "fishhook.h"

#include <dlfcn.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <mach/mach.h>
#include <mach/vm_map.h>
#include <mach/vm_region.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>

#ifdef __LP64__
typedef struct mach_header_64 mach_header_t;
typedef struct segment_command_64 segment_command_t;
typedef struct section_64 section_t;
typedef struct nlist_64 nlist_t;
#define LC_SEGMENT_ARCH_DEPENDENT LC_SEGMENT_64
#else
typedef struct mach_header mach_header_t;
typedef struct segment_command segment_command_t;
typedef struct section section_t;
typedef struct nlist nlist_t;
#define LC_SEGMENT_ARCH_DEPENDENT LC_SEGMENT
#endif

#ifndef SEG_DATA_CONST
#define SEG_DATA_CONST  "__DATA_CONST"
#endif

// 链表
struct rebindings_entry {
  struct rebinding *rebindings; // 数组实例
  size_t rebindings_nel; // 数量
  struct rebindings_entry *next; // 下一个
};

// 全局量，表头
static struct rebindings_entry *_rebindings_head;

// 维护 rebindings_entry 链表
static int prepend_rebindings(struct rebindings_entry **rebindings_head,
                              struct rebinding rebindings[],
                              size_t nel) {
  struct rebindings_entry *new_entry = (struct rebindings_entry *) malloc(sizeof(struct rebindings_entry));
  if (!new_entry) {
    return -1;
  }
  new_entry->rebindings = (struct rebinding *) malloc(sizeof(struct rebinding) * nel);
  if (!new_entry->rebindings) {
    free(new_entry);
    return -1;
  }
  memcpy(new_entry->rebindings, rebindings, sizeof(struct rebinding) * nel);
  new_entry->rebindings_nel = nel;
  new_entry->next = *rebindings_head;
  *rebindings_head = new_entry;
  return 0;
}

static vm_prot_t get_protection(void *sectionStart) {
  mach_port_t task = mach_task_self();
  vm_size_t size = 0;
  vm_address_t address = (vm_address_t)sectionStart;
  memory_object_name_t object;
#if __LP64__
  mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
  vm_region_basic_info_data_64_t info;
  kern_return_t info_ret = vm_region_64(
      task, &address, &size, VM_REGION_BASIC_INFO_64, (vm_region_info_64_t)&info, &count, &object);
#else
  mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT;
  vm_region_basic_info_data_t info;
  kern_return_t info_ret = vm_region(task, &address, &size, VM_REGION_BASIC_INFO, (vm_region_info_t)&info, &count, &object);
#endif
  if (info_ret == KERN_SUCCESS) {
    return info.protection;
  } else {
    return VM_PROT_READ;
  }
}

/// 重新绑定
static void perform_rebinding_with_section(struct rebindings_entry *rebindings,
                                           section_t *section,
                                           intptr_t slide,
                                           nlist_t *symtab,
                                           char *strtab,
                                           uint32_t *indirect_symtab) {
  // 常量
  const bool isDataConst = strcmp(section->segname, SEG_DATA_CONST) == 0;
  // 在 indirect symbol 表中找到对应的位置
  uint32_t *indirect_symbol_indices = indirect_symtab + section->reserved1;
  // 获取 __DATA.__nl_symbol_ptr(或者__la_symbol_ptr) section
  // 已知其 value 是一个指针类型。整个区域用二阶指针来获取
  void **indirect_symbol_bindings = (void **)((uintptr_t)slide + section->addr);
  vm_prot_t oldProtection = VM_PROT_READ;
  if (isDataConst) {
    oldProtection = get_protection(rebindings);
    // mprotect()函数可以用来修改一段指定内存区域的保护属性。
    mprotect(indirect_symbol_bindings, section->size, PROT_READ | PROT_WRITE);
  }
  // 遍历整个 section
  for (uint i = 0; i < section->size / sizeof(void *); i++) {
    // 通过下标获取每一个 indirect address 的值
    uint32_t symtab_index = indirect_symbol_indices[i];
    if (symtab_index == INDIRECT_SYMBOL_ABS || symtab_index == INDIRECT_SYMBOL_LOCAL ||
        symtab_index == (INDIRECT_SYMBOL_LOCAL   | INDIRECT_SYMBOL_ABS)) {
      continue;
    }
    // 获取符号名在表中的偏移地址
    uint32_t strtab_offset = symtab[symtab_index].n_un.n_strx;
    // 获取符号名
    char *symbol_name = strtab + strtab_offset;
    // 符号名大于1
    bool symbol_name_longer_than_1 = symbol_name[0] && symbol_name[1];
    //  取出 rebinding 结构体实例数组，开始遍历
    struct rebindings_entry *cur = rebindings;
    while (cur) {
      // 对于链表中每一个 rebindings 数组的每一个 rebinding 实例 依次在 String Table 匹配符号
      for (uint j = 0; j < cur->rebindings_nel; j++) {
        // 符号名与方法名匹配
        if (symbol_name_longer_than_1 &&
            strcmp(&symbol_name[1], cur->rebindings[j].name) == 0) {
          // 如果是第一次对跳转地址进行重写
          if (cur->rebindings[j].replaced != NULL &&
              indirect_symbol_bindings[i] != cur->rebindings[j].replacement) {
            // 记录原始跳转地址
            *(cur->rebindings[j].replaced) = indirect_symbol_bindings[i];
          }
          // 重写跳转地址
          indirect_symbol_bindings[i] = cur->rebindings[j].replacement;
          // 退出遍历，即针对同一符号多次调用fishhook重绑定，只有对最后一次调用生效
          goto symbol_loop;
        }
      }
      cur = cur->next;
    }
  symbol_loop:;
  }
  if (isDataConst) {
    int protection = 0;
    if (oldProtection & VM_PROT_READ) {
      protection |= PROT_READ;
    }
    if (oldProtection & VM_PROT_WRITE) {
      protection |= PROT_WRITE;
    }
    if (oldProtection & VM_PROT_EXECUTE) {
      protection |= PROT_EXEC;
    }
    mprotect(indirect_symbol_bindings, section->size, protection);
  }
}

/*
 核心方法
 */
static void rebind_symbols_for_image(struct rebindings_entry *rebindings,
                                     const struct mach_header *header,
                                     intptr_t slide) {
  Dl_info info;
  if (dladdr(header, &info) == 0) {
    return;
  }
 
  // 声明几个查找量:
  // linkedit_segment, symtab_command, dysymtab_command
  segment_command_t *cur_seg_cmd;
  segment_command_t *linkedit_segment = NULL;
  struct symtab_command* symtab_cmd = NULL;
  struct dysymtab_command* dysymtab_cmd = NULL;

  // 初始化游标
  // header = 0x100000000 - 二进制文件基址默认偏移
  // sizeof(mach_header_t) = 0x20 - Mach-O Header 部分
  // 首先需要跳过 Mach-O Header
  uintptr_t cur = (uintptr_t)header + sizeof(mach_header_t);
  // 遍历每一个 Load Command，游标每一次偏移每个命令的 Command Size 大小
  // header -> ncmds: Load Command 加载命令数量
  // cur_seg_cmd -> cmdsize: Load 大小
  for (uint i = 0; i < header->ncmds; i++, cur += cur_seg_cmd->cmdsize) {
    // 取出当前 Load Command
    cur_seg_cmd = (segment_command_t *)cur;
    // LC_SEGMENT Command
    if (cur_seg_cmd->cmd == LC_SEGMENT_ARCH_DEPENDENT) {
      // SEG_LINKEDIT Command
      if (strcmp(cur_seg_cmd->segname, SEG_LINKEDIT) == 0) {
        // 查找到 __LINKEDIT
        linkedit_segment = cur_seg_cmd;
      }
    } else if (cur_seg_cmd->cmd == LC_SYMTAB) {
      // 找到 LC_SEGMENT
      symtab_cmd = (struct symtab_command*)cur_seg_cmd;
    } else if (cur_seg_cmd->cmd == LC_DYSYMTAB) {
      // 找到 LC_DYSYMTAB
      dysymtab_cmd = (struct dysymtab_command*)cur_seg_cmd;
    }
  }

  // 容错
  if (!symtab_cmd || !dysymtab_cmd || !linkedit_segment ||
      !dysymtab_cmd->nindirectsyms) {
    return;
  }

  // Find base symbol/string table addresses
  // slide: ASLR 偏移量
  // vmaddr: SEG_LINKEDIT 虚拟地址
  // fileoff: SEG_LINKEDIT 偏移量
  // base = SEG_LINKDIT 真实地址 - SEG_LINKDIT 偏移量
  // SEG_LINKDIT 真实地址 = ASLR 偏移量 + SEG_LINKEDIT 虚拟地址
  uintptr_t linkedit_base = (uintptr_t)slide + linkedit_segment->vmaddr - linkedit_segment->fileoff;
  // 通过 base + symoff 偏移量 计算出 symtab 表的首地址，并获取 nlist_t 结构体实例
  nlist_t *symtab = (nlist_t *)(linkedit_base + symtab_cmd->symoff);
  // 通过 base + stroff 偏移量 计算出字符表中的首地址，并获取字符串表
  char *strtab = (char *)(linkedit_base + symtab_cmd->stroff);
  // Get indirect symbol table (array of uint32_t indices into symbol table)
  // 通过 base + indirectsymoff 偏移量来计算动态符号表的首地址
  uint32_t *indirect_symtab = (uint32_t *)(linkedit_base + dysymtab_cmd->indirectsymoff);

  // 游标归零（复用）
  cur = (uintptr_t)header + sizeof(mach_header_t);
  // 遍历 Load Command
  for (uint i = 0; i < header->ncmds; i++, cur += cur_seg_cmd->cmdsize) {
    cur_seg_cmd = (segment_command_t *)cur;
    if (cur_seg_cmd->cmd == LC_SEGMENT_ARCH_DEPENDENT) {
      // 通过 segname 过滤出 __DATA 或者 __DATA_CONST
      // strcmp 比较字符串
      if (strcmp(cur_seg_cmd->segname, SEG_DATA) != 0 &&
          strcmp(cur_seg_cmd->segname, SEG_DATA_CONST) != 0) {
        continue;
      }
      // 遍历 segment 中的 section
      for (uint j = 0; j < cur_seg_cmd->nsects; j++) {
        // 取出 section
        section_t *sect =
          (section_t *)(cur + sizeof(segment_command_t)) + j;
        // flags & SECTION_TYPE 通过 SECTION_TYPE 掩码获取 flags 记录类型的 8 bit
        // S_LAZY_SYMOBL_POINTERS 表示 lazy symbol 指针 section
        if ((sect->flags & SECTION_TYPE) == S_LAZY_SYMBOL_POINTERS) {
          // 进行 rebinding 重新操作
          perform_rebinding_with_section(rebindings, sect, slide, symtab, strtab, indirect_symtab);
        }
        // S_NON_LAZY_SYMBOL_POINTERS 表示 non-lazy symbol 指针 section
        if ((sect->flags & SECTION_TYPE) == S_NON_LAZY_SYMBOL_POINTERS) {
          perform_rebinding_with_section(rebindings, sect, slide, symtab, strtab, indirect_symtab);
        }
      }
    }
  }
}

/*
 _rebind_symbols_for_image 是 rebind_symbols_image 的一个入口方法
 这个入口方法是为了满足 _dyld_register_func_for_add_image 传入回调方法的格式
 
 header - Mach-O header
 slide - intptr_t 持有指针
 */
static void _rebind_symbols_for_image(const struct mach_header *header,
                                      intptr_t slide) {
    rebind_symbols_for_image(_rebindings_head, header, slide);
}

int rebind_symbols_image(void *header,
                         intptr_t slide,
                         struct rebinding rebindings[],
                         size_t rebindings_nel) {
    struct rebindings_entry *rebindings_head = NULL;
    int retval = prepend_rebindings(&rebindings_head, rebindings, rebindings_nel);
    rebind_symbols_for_image(rebindings_head, (const struct mach_header *) header, slide);
    if (rebindings_head) {
      free(rebindings_head->rebindings);
    }
    free(rebindings_head);
    return retval;
}

int rebind_symbols(struct rebinding rebindings[], size_t rebindings_nel) {
  // 维护一个 rebinding_entry 的结构
  // 将多个 rebinding 的多个实例组织成一个链表
  int retval = prepend_rebindings(&_rebindings_head, rebindings, rebindings_nel);
  // malloc 失败返回 -1
  if (retval < 0) {
    return retval;
  }
  // If this was the first call, register callback for image additions (which is also invoked for
  // existing images, otherwise, just run on existing images
  if (!_rebindings_head->next) {
    // 第一次调用，将 _rebind_symbols_for_image 注册为回调
    /*
     _dyld_register_func_for_add_image 这个方法当镜像 Image 被 load 或是 unload 的时候都会由
     dyld 主动调用。当该方法被触发时，会为每个镜像触发其回调方法。
     之后则将其镜像与其回调函数进行绑定（但是未进行初始化）。
     使用 _dyld_register_func_for_add_image 注册的回调将在镜像中的
     terminators 启动后被调用。
     */
    _dyld_register_func_for_add_image(_rebind_symbols_for_image);
  } else {
    // 先获取 dyld 镜像数量
    uint32_t c = _dyld_image_count();
    for (uint32_t i = 0; i < c; i++) {
      // 根据下标依次进行重新绑定
      _rebind_symbols_for_image(_dyld_get_image_header(i), _dyld_get_image_vmaddr_slide(i));
    }
  }
  return retval;
}
