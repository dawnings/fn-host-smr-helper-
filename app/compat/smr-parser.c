#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdint.h>
#include <link.h>
#include <mntent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
__attribute__((constructor)) static void main_only(void){
 const char *p=getenv("LD_PRELOAD");
 if(p&&strstr(p,"smr-parser")&&program_invocation_short_name&&!strcmp(program_invocation_short_name,"trim"))unsetenv("LD_PRELOAD");
}
static __thread char physical[64];
static int identity(const char *disk,const char *serial,const char *id){
 char path[256],line[512],needle[128];FILE*f;
 snprintf(path,sizeof(path),"/sys/class/block/%s/device",disk);
 char real[PATH_MAX];if(!realpath(path,real))return 0;
 snprintf(path,sizeof(path),"/sys/class/block/%s/queue/zoned",disk);
 f=fopen(path,"r");if(!f)return 0;int zoned=fgets(line,sizeof(line),f)&&!strcmp(line,"host-managed\n");fclose(f);if(!zoned)return 0;
 snprintf(path,sizeof(path),"/sys/class/block/%s/dev",disk);
 f=fopen(path,"r");if(!f)return 0;if(!fgets(line,sizeof(line),f)){fclose(f);return 0;}fclose(f);
 line[strcspn(line,"\n")]=0;if(strlen(line)>32)return 0;snprintf(path,sizeof(path),"/run/udev/data/b%.32s",line);
 f=fopen(path,"r");if(!f)return 0;int ok=0;
 snprintf(needle,sizeof(needle),"E:ID_SERIAL_SHORT=%s\n",serial);
 while(fgets(line,sizeof(line),f))if(!strcmp(line,needle))ok=1;
 fclose(f);
 if(!realpath(id,real)||strcmp(strrchr(real,'/')+1,disk))return 0;
 return ok;
}
/* Only these two read-only storage discovery call sites may see the raw disk. */
static int storage_build(struct dl_phdr_info *info, size_t size, void *base) {
 (void)size;
 if (info->dlpi_addr != (uintptr_t)base) return 0;
 static const unsigned char build_id[] = {
  0x8b, 0x29, 0x14, 0xff, 0xef, 0xb5, 0xdc, 0xe7, 0xd0, 0x1a,
  0x6d, 0x01, 0xf4, 0x77, 0xf9, 0x5e, 0xc6, 0x53, 0xb8, 0xfb
 };
 for (int i = 0; i < info->dlpi_phnum; ++i) {
  const ElfW(Phdr) *ph = &info->dlpi_phdr[i];
  if (ph->p_type != PT_NOTE || ph->p_filesz != sizeof(ElfW(Nhdr)) + 4 + sizeof(build_id))
   continue;
  const ElfW(Nhdr) *note = (const void *)(info->dlpi_addr + ph->p_vaddr);
  const char *owner = (const char *)(note + 1);
  if (note->n_type == NT_GNU_BUILD_ID && note->n_namesz == 4 &&
      note->n_descsz == sizeof(build_id) && !memcmp(owner, "GNU", 4) &&
      !memcmp(owner + 4, build_id, sizeof(build_id)))
   return 1;
 }
 return 0;
}

static int storage_callsite(void *ret, uintptr_t expected) {
 Dl_info info;
 if (!dladdr(ret, &info) || !info.dli_fname ||
     strcmp(info.dli_fname, "/usr/trim/bin/handlers/storage.hdl"))
  return 0;
 uintptr_t offset = (uintptr_t)ret - (uintptr_t)info.dli_fbase;
 return offset == expected &&
        dl_iterate_phdr(storage_build, info.dli_fbase) == 1;
}

static const char *registered_disk(const char *p, char *verified_serial){
 const char *ordinary = NULL;
 char serial[65],dmName[128],vg[128],uuid[64],id[256],line[1024],conf[512];
 const char *var=getenv("SMR_VAR");if(!var)var="/var/apps/smr-manager/var";
 snprintf(conf,sizeof(conf),"%s/association.enabled",var);if(access(conf,F_OK))return (char*)ordinary;
 snprintf(conf,sizeof(conf),"%s/native.conf",var);FILE *cf=fopen(conf,"r");if(!cf)return (char*)ordinary;
 int found=0;while(fgets(line,sizeof(line),cf)){if(sscanf(line,"%64s %127s %127s %63s %255s",serial,dmName,vg,uuid,id)!=5)continue;char expectedPV[256];snprintf(expectedPV,sizeof(expectedPV),"/dev/mapper/%s",dmName);if(!strcmp(p,expectedPV)){found=1;break;}}fclose(cf);if(!found)return (char*)ordinary;
 char real[PATH_MAX],path[256];
 if(!realpath(p,real))return (char*)ordinary;
 const char *dm=strrchr(real,'/');if(!dm||strncmp(++dm,"dm-",3))return (char*)ordinary;
 // Expected single-device dm-zoned mapping: use the kernel's actual slave list.
 snprintf(path,sizeof(path),"/sys/class/block/%s/slaves",dm);
 DIR *dir=opendir(path);if(!dir)return (char*)ordinary;
 struct dirent *entry;int count=0;
 while((entry=readdir(dir)))if(entry->d_name[0]!='.')count++;
 closedir(dir);if(count!=1)return (char*)ordinary;
 // readlink of the known raw device plus holder membership avoids directory traversal helpers.
 snprintf(path,sizeof(path),"%s",id);
 if(!realpath(path,real))return (char*)ordinary;
 const char *disk=strrchr(real,'/');if(!disk||strlen(++disk)>=sizeof(physical)||!identity(disk,serial,id))return (char*)ordinary;
 char raw[64];strcpy(raw,disk);
 char mapped[PATH_MAX];if(!realpath(p,mapped))return (char*)ordinary;
 dm=strrchr(mapped,'/')+1;
 snprintf(path,sizeof(path),"/sys/class/block/%s/slaves/%s",dm,raw);
 if(access(path,F_OK))return (char*)ordinary;
 snprintf(path,sizeof(path),"/sys/class/block/%s/dm/name",dm);
 FILE*f=fopen(path,"r");if(!f)return (char*)ordinary;
 char name[128],expected[130];snprintf(expected,sizeof(expected),"%s\n",dmName);
 int ok=fgets(name,sizeof(name),f)&&!strcmp(name,expected);fclose(f);
 if(!ok)return (char*)ordinary;
 if (verified_serial) strcpy(verified_serial, serial);
 strcpy(physical,raw);return physical;
}


char *basename(const char *path) {
 if (!*path) return ".";
 const char *slash = strrchr(path, '/');
 if (!strncmp(path, "/dev/mapper/", 12) &&
     storage_callsite(__builtin_return_address(0), 0x9b78f)) {
  const char *disk = registered_disk(path, NULL);
  if (disk) return (char *)disk;
 }
 return (char *)(slash ? slash + 1 : path);
}

static int dm_device(const char *name) {
 return !strncmp(name, "dm-", 3) && name[3] &&
        strspn(name + 3, "0123456789") == strlen(name + 3);
}

static int attribute(const char *device, const char *key, char *value, size_t capacity) {
 char path[256];
 int length = snprintf(path, sizeof(path), "/sys/class/block/%s/%s", device, key);
 if (length < 0 || (size_t)length >= sizeof(path)) return 0;
 FILE *file = fopen(path, "r");
 if (!file) return 0;
 int ok = fgets(value, capacity, file) != NULL;
 fclose(file);
 if (!ok) return 0;
 value[strcspn(value, "\r\n")] = '\0';
 return *value != '\0';
}

static const char *linear_member(const char *device, const char *mapper, char serial[65]) {
 if (!dm_device(device)) return NULL;
 char value[160];
 if (!attribute(device, "dm/uuid", value, sizeof(value)) || strncmp(value, "LVM-", 4))
  return NULL;
 if (!attribute(device, "dm/name", value, sizeof(value)) || strcmp(value, mapper))
  return NULL;

 char path[256], slave[64] = "";
 snprintf(path, sizeof(path), "/sys/class/block/%s/slaves", device);
 DIR *dir = opendir(path);
 if (!dir) return NULL;
 int count = 0;
 struct dirent *entry;
 while ((entry = readdir(dir))) {
  if (entry->d_name[0] == '.') continue;
  ++count;
  if (strlen(entry->d_name) < sizeof(slave)) strcpy(slave, entry->d_name);
 }
 closedir(dir);
 if (count != 1 || !dm_device(slave) ||
     !attribute(slave, "dm/name", value, sizeof(value)) || strncmp(value, "SMR_", 4) ||
     strspn(value, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_.+-") != strlen(value))
  return NULL;
 snprintf(path, sizeof(path), "/dev/mapper/%s", value);
 return registered_disk(path, serial);
}

static unsigned long block_number(const char *disk, const char *key) {
 char value[64], *end;
 if (!attribute(disk, key, value, sizeof(value))) return 0;
 errno = 0;
 unsigned long number = strtoul(value, &end, 10);
 return !errno && !*end ? number : 0;
}

struct json_api {
 void *(*key)(void *, const char *);
 void *(*index)(void *, unsigned long);
 size_t (*size)(void *);
 void (*create)(void *, void *);
 void (*destroy)(void *);
 void *(*string)(void *, const char *);
 void *(*number)(void *, unsigned long);
 void (*append)(void *, void *);
};

static const char *json_string(void *value) {
 /* Build ID above guards PPJson's virtual as-string slot. */
 void **vtable = *(void ***)value;
 const char *(*read_string)(void *, const char *) = vtable[0x98 / sizeof(void *)];
 return read_string(value, "");
}

static struct json_api json_api(void) {
 return (struct json_api) {
  dlsym(RTLD_NEXT, "_ZN6PPJson8MutValueixEPKc"),
  dlsym(RTLD_NEXT, "_ZN6PPJson8MutValueixEm"),
  dlsym(RTLD_NEXT, "_ZN6PPJson8MutValue4sizeEv"),
  dlsym(RTLD_NEXT, "_ZN6PPJson8MutValueC1ERNS_11MutDocumentE"),
  dlsym(RTLD_NEXT, "_ZN6PPJson8MutValueD1Ev"),
  dlsym(RTLD_NEXT, "_ZN6PPJson8MutValueaSEPKc"),
  dlsym(RTLD_NEXT, "_ZN6PPJson8MutValueaSEm"),
  dlsym(RTLD_NEXT, "_ZN6PPJson8MutValue6appendERS0_"),
 };
}

static int json_ready(struct json_api api) {
 return api.key && api.index && api.size && api.create && api.destroy &&
        api.string && api.number && api.append;
}

static void populate_members(void *doc, void *root) {
 const struct json_api api = json_api();
 if (!json_ready(api)) return;

 void *volumes = api.key(root, "array");
 for (size_t i = 0; i < api.size(volumes); ++i) {
  void *volume = api.index(volumes, i);
  if (strcmp(json_string(api.key(volume, "level")), "linear")) continue;
  char serial[65];
  const char *disk = linear_member(json_string(api.key(volume, "name")),
                                   json_string(api.key(volume, "uuid")), serial);
  if (!disk) continue;

  void *members = api.key(volume, "disk");
  if (api.size(members)) continue;
  char model[128];
  unsigned long sectors = block_number(disk, "size");
  if (!sectors || sectors > ULONG_MAX / 512 ||
      !attribute(disk, "device/model", model, sizeof(model))) continue;
  size_t length = strlen(model);
  while (length && model[length - 1] == ' ') model[--length] = '\0';

  /* The same build creates temporary MutValue instances in 0x60 bytes. */
  _Alignas(16) unsigned char member[256];
  api.create(member, doc);
  api.string(api.key(member, "name"), disk);
  api.string(api.key(member, "serialNumber"), serial);
  api.string(api.key(member, "modelName"), model);
  api.string(api.key(member, "type"), "HDD");
  api.string(api.key(member, "protocol"), "SATA");
  api.number(api.key(member, "size"), sectors * 512);
  api.number(api.key(member, "logicalBlockSize"), block_number(disk, "queue/logical_block_size"));
  api.number(api.key(member, "physicalBlockSize"), block_number(disk, "queue/physical_block_size"));
  api.append(members, member);
  api.destroy(member);
 }
}

/* Resource Monitor renders disk[].storage from the first disk-list reply. */
static void populate_usage(void *doc, void *root) {
 const struct json_api api = json_api();
 if (!json_ready(api)) return;
 FILE *mounts = setmntent("/proc/self/mounts", "r");
 if (!mounts) return;
 void *disks = api.key(root, "disk");
 struct mntent mount;
 char buffer[8192];
 while (getmntent_r(mounts, &mount, buffer, sizeof(buffer))) {
  const char *dir = mount.mnt_dir, *source = mount.mnt_fsname;
  if (strncmp(dir, "/vol", 4) || dir[4] < '1' || dir[4] > '9' ||
      strspn(dir + 4, "0123456789") != strlen(dir + 4) ||
      strcmp(mount.mnt_type, "btrfs") || strncmp(source, "/dev/mapper/trim_", 17))
   continue;
  errno = 0;
  unsigned long id = strtoul(dir + 4, NULL, 10);
  if (errno || id > UINT_MAX) continue;
  char resolved[PATH_MAX], serial[65];
  if (!realpath(source, resolved)) continue;
  const char *device = strrchr(resolved, '/');
  if (!device) continue;
  const char *mapper = source + 12;
  const char *disk = linear_member(device + 1, mapper, serial);
  if (!disk) continue;

  for (size_t i = 0; i < api.size(disks); ++i) {
   void *record = api.index(disks, i);
   if (strcmp(json_string(api.key(record, "name")), disk)) continue;
   const char *listed_serial = json_string(api.key(record, "serialNumber"));
   if (*listed_serial && strcmp(listed_serial, serial)) continue;
   void *storage = api.key(record, "storage");
   if (api.size(storage)) continue;
   _Alignas(16) unsigned char member[256];
   api.create(member, doc);
   api.number(api.key(member, "storId"), id);
   api.string(api.key(member, "storUuid"), mapper);
   api.string(api.key(member, "storName"), device + 1);
   api.string(api.key(member, "arrState"), "in_sync");
   api.string(api.key(member, "level"), "linear");
   api.append(storage, member);
   api.destroy(member);
  }
 }
 endmntent(mounts);
}

static __thread void *disk_list_reply;

void storage_json_remove(void *, const char *)
 __asm__("_ZN6PPJson8MutValue6removeEPKc");

void storage_json_remove(void *value, const char *key) {
 void (*original)(void *, const char *) =
  dlsym(RTLD_NEXT, "_ZN6PPJson8MutValue6removeEPKc");
 original(value, key);
 /* The fast disk-list path removes md immediately before returning its reply. */
 if (storage_callsite(__builtin_return_address(0), 0x84c02))
  disk_list_reply = value;
}

char *storage_json_write(void *, void *, unsigned long *)
 __asm__("_ZN6PPJson11MutDocument5writeERNS_8MutValueEPm");

char *storage_json_write(void *doc, void *root, unsigned long *length) {
 char *(*original)(void *, void *, unsigned long *) =
  dlsym(RTLD_NEXT, "_ZN6PPJson11MutDocument5writeERNS_8MutValueEPm");
 void *caller = __builtin_return_address(0);
 if (storage_callsite(caller, 0x4e9ae))
  populate_members(doc, root);
 else if (storage_callsite(caller, 0x848c5))
  populate_usage(doc, root);
 else if (disk_list_reply && storage_callsite(caller, 0x2ddab)) {
  void *reply = disk_list_reply;
  disk_list_reply = NULL;
  if (reply == root) populate_usage(doc, root);
 }
 return original(doc, root, length);
}
