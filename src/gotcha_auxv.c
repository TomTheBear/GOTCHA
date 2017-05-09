/*
This file is part of GOTCHA.  For copyright information see the COPYRIGHT
file in the top level directory, or at
https://github.com/LLNL/gotcha/blob/master/COPYRIGHT
This program is free software; you can redistribute it and/or modify it under
the terms of the GNU Lesser General Public License (as published by the Free
Software Foundation) version 2.1 dated February 1999.  This program is
distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
without even the IMPLIED WARRANTY OF MERCHANTABILITY or FITNESS FOR A PARTICULAR
PURPOSE. See the terms and conditions of the GNU Lesser General Public License
for more details.  You should have received a copy of the GNU Lesser General
Public License along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
*/

#include "gotcha_auxv.h"
#include "gotcha_utils.h"
#include "libc_wrappers.h"

#include <elf.h>
#include <link.h>
#include <stdio.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>


static ElfW(Ehdr) *vdso_ehdr = NULL;
static int auxv_pagesz = 0;

static int parse_auxv_contents()
{
   char name[] = "/proc/self/auxv";
   int fd, done = 0;
   char buffer[4096];
   ssize_t buffer_size = 4096, offset = 0, result;
   ElfW(auxv_t) *auxv, *a;
   static int parsed_auxv = 0;

   if (parsed_auxv)
      return parsed_auxv == -1 ? parsed_auxv : 0;
   parsed_auxv = 1;

   fd = gotcha_open(name, O_RDONLY);
   if (fd == -1) {
      parsed_auxv = -1;
      return -1;
   }

   do {
      for (;;) {
         result = gotcha_read(fd, buffer+offset, buffer_size-offset);
         if (result == -1) {
            if (errno == EINTR)
               continue;
            gotcha_close(fd);
            parsed_auxv = -1;
            return -1;
         }
         if (result == 0) {
            gotcha_close(fd);
            done = 1;
            break;
         }
         if (offset == buffer_size) {
            break;
         }
         offset += result;
      }

      auxv = (ElfW(auxv_t) *) buffer;
      for (a = auxv; a->a_type != AT_NULL; a++) {
         if (a->a_type == AT_SYSINFO_EHDR) {
            vdso_ehdr = (ElfW(Ehdr) *) a->a_un.a_val;
         }
         else if (a->a_type == AT_PAGESZ) {
            auxv_pagesz = (int) a->a_un.a_val;
         }
      }
   } while (!done);

   return 0;
}

static struct link_map *get_vdso_from_auxv()
{
   struct link_map *m;

   ElfW(Phdr) *vdso_phdrs = NULL;
   ElfW(Half) vdso_phdr_num, p;
   ElfW(Addr) vdso_dynamic;

   parse_auxv_contents();
   if (!vdso_ehdr)
      return NULL;
   
   vdso_phdrs = (ElfW(Phdr) *) (vdso_ehdr->e_phoff + ((unsigned char *) vdso_ehdr));
   vdso_phdr_num = vdso_ehdr->e_phnum;

   for (p = 0; p < vdso_phdr_num; p++) {
      if (vdso_phdrs[p].p_type == PT_DYNAMIC) {
         vdso_dynamic = (ElfW(Addr)) vdso_phdrs[p].p_vaddr;
      }
   }

   for (m = _r_debug.r_map; m; m = m->l_next) {
      if (m->l_addr + vdso_dynamic == (ElfW(Addr)) m->l_ld) {
         return m;
      }
   }
   return NULL;
}

int get_auxv_pagesize()
{
   int result;
   result = parse_auxv_contents();
   if (result == -1)
      return 0;
   return auxv_pagesz;
}

static char* vdso_aliases[] = { "linux-vdso.so",
                                "linux-gate.so",
                                NULL };

static struct link_map *get_vdso_from_aliases()
{
   struct link_map *m;
   char **aliases;

   for (m = _r_debug.r_map; m; m = m->l_next) {
      for (aliases = vdso_aliases; *aliases; aliases++) {
         if (m->l_name && strcmp(m->l_name, *aliases) == 0) {
            return m;
         }
      }
   }
   return NULL;
}

static struct link_map *get_vdso_from_maps()
{
   FILE *maps = fopen("/proc/self/maps", "r");
   ElfW(Addr) addr_begin, addr_end, dynamic;
   char name[4096], line[4096];
   struct link_map *m;
   int result;
   
   for (;;) {
      fgets(line, 4097, maps);
      result = sscanf(line, "%lx-%lx %*s %*s %*s %*s %4096s\n", &addr_begin, &addr_end, name);
      if (result != 3) {
         continue;
      }
      if (strcmp(name, "[vdso]") == 0) {
         fclose(maps);
         break;
      }
      if (feof(maps)) {
         fclose(maps);
         return NULL;
      }
   }

   for (m = _r_debug.r_map; m; m = m->l_next) {
      dynamic = (ElfW(Addr)) m->l_ld;
      if (dynamic >= addr_begin && dynamic < addr_end)
         return m;
   }
   
   return NULL;
}

int is_vdso(struct link_map *map)
{
   static int vdso_checked = 0;
   static struct link_map *vdso = NULL;
   struct link_map *result;

   if (!map)
      return 0;
   if (vdso_checked)
      return (map == vdso);
   
   vdso_checked = 1;

   result = get_vdso_from_aliases();
   if (result) {
      vdso = result;
      return (map == vdso);
   }

   result = get_vdso_from_auxv();
   if (result) {
      vdso = result;
      return (map == vdso);
   }

   result = get_vdso_from_maps();
   if (result) {
      vdso = result;
      return (map == vdso);
   }

   return 0;
}
