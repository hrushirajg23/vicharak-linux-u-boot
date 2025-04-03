/*
 * Copyright (c) 2012, NVIDIA CORPORATION.  All rights reserved.
 *
 * SPDX-License-Identifier:	GPL-2.0
 */

#include <config.h>
#include <errno.h>
#include <common.h>
#include <mapmem.h>
#include <part.h>
#include <ext4fs.h>
#include <fat.h>
#include <fs.h>
#include <sandboxfs.h>
#include <ubifs_uboot.h>
#include <asm/io.h>
#include <div64.h>
#include <linux/math64.h>
#include <efuse.h>
#include <u-boot/rsa.h>
#include <u-boot/sha256.h>
#include <hash.h>	
#include <malloc.h>

DECLARE_GLOBAL_DATA_PTR;

#define SIGNATURE_SIZE 256

static struct blk_desc *fs_dev_desc;
static int fs_dev_part;
static disk_partition_t fs_partition;
static int fs_type = FS_TYPE_ANY;
const char *kernel_image="/Image-5.10.230-vaaman";
static char buffer[128];
static u32 integer_array[32];
static int boot_efuse_read(struct udevice *dev, int offset,void *buf, int size);
static int boot_efuse_write(struct udevice *dev, int offset,void *buf, int size);
int read_public_key(char* ptr);
int write_public_key(char* ptr);
void fs_read_into_buff(const char *filename,void* buf, ulong addr, loff_t offset, loff_t len,
	loff_t *actread);

	// struct key_prop {
	// 	uint8_t key[256]; // Assuming a 2048-bit RSA key
	//  };

// void compute_sha256(const void *data, size_t len, uint8_t *out_hash) {
// 	sha256_context ctx;
// 	sha256_starts(&ctx);
// 	sha256_update(&ctx, data, len);
// 	sha256_finish(&ctx, out_hash);
// 	}
static inline int fs_probe_unsupported(struct blk_desc *fs_dev_desc,
				      disk_partition_t *fs_partition)
{
	printf("** Unrecognized filesystem type **\n");
	return -1;
}

static inline int fs_ls_unsupported(const char *dirname)
{
	return -1;
}

/* generic implementation of ls in terms of opendir/readdir/closedir */
__maybe_unused
static int fs_ls_generic(const char *dirname)
{
	struct fs_dir_stream *dirs;
	struct fs_dirent *dent;
	int nfiles = 0, ndirs = 0;

	dirs = fs_opendir(dirname);
	if (!dirs)
		return -errno;

	while ((dent = fs_readdir(dirs))) {
		if (dent->type == FS_DT_DIR) {
			printf("            %s/\n", dent->name);
			ndirs++;
		} else {
			printf(" %8lld   %s\n", dent->size, dent->name);
			nfiles++;
		}
	}

	fs_closedir(dirs);

	printf("\n%d file(s), %d dir(s)\n\n", nfiles, ndirs);

	return 0;
}

static inline int fs_exists_unsupported(const char *filename)
{
	return 0;
}

static inline int fs_size_unsupported(const char *filename, loff_t *size)
{
	return -1;
}

static inline int fs_read_unsupported(const char *filename, void *buf,
				      loff_t offset, loff_t len,
				      loff_t *actread)
{
	return -1;
}

static inline int fs_write_unsupported(const char *filename, void *buf,
				      loff_t offset, loff_t len,
				      loff_t *actwrite)
{
	return -1;
}

static inline void fs_close_unsupported(void)
{
}

static inline int fs_uuid_unsupported(char *uuid_str)
{
	return -1;
}

static inline int fs_opendir_unsupported(const char *filename,
					 struct fs_dir_stream **dirs)
{
	return -EACCES;
}

struct fstype_info {
	int fstype;
	char *name;
	/*
	 * Is it legal to pass NULL as .probe()'s  fs_dev_desc parameter? This
	 * should be false in most cases. For "virtual" filesystems which
	 * aren't based on a U-Boot block device (e.g. sandbox), this can be
	 * set to true. This should also be true for the dumm entry at the end
	 * of fstypes[], since that is essentially a "virtual" (non-existent)
	 * filesystem.
	 */
	bool null_dev_desc_ok;
	int (*probe)(struct blk_desc *fs_dev_desc,
		     disk_partition_t *fs_partition);
	int (*ls)(const char *dirname);
	int (*exists)(const char *filename);
	int (*size)(const char *filename, loff_t *size);
	int (*read)(const char *filename, void *buf, loff_t offset,
		    loff_t len, loff_t *actread);
	int (*write)(const char *filename, void *buf, loff_t offset,
		     loff_t len, loff_t *actwrite);
	void (*close)(void);
	int (*uuid)(char *uuid_str);
	/*
	 * Open a directory stream.  On success return 0 and directory
	 * stream pointer via 'dirsp'.  On error, return -errno.  See
	 * fs_opendir().
	 */
	int (*opendir)(const char *filename, struct fs_dir_stream **dirsp);
	/*
	 * Read next entry from directory stream.  On success return 0
	 * and directory entry pointer via 'dentp'.  On error return
	 * -errno.  See fs_readdir().
	 */
	int (*readdir)(struct fs_dir_stream *dirs, struct fs_dirent **dentp);
	/* see fs_closedir() */
	void (*closedir)(struct fs_dir_stream *dirs);
};

static struct fstype_info fstypes[] = {
#ifdef CONFIG_FS_FAT
	{
		.fstype = FS_TYPE_FAT,
		.name = "fat",
		.null_dev_desc_ok = false,
		.probe = fat_set_blk_dev,
		.close = fat_close,
		.ls = fs_ls_generic,
		.exists = fat_exists,
		.size = fat_size,
		.read = fat_read_file,
#ifdef CONFIG_FAT_WRITE
		.write = file_fat_write,
#else
		.write = fs_write_unsupported,
#endif
		.uuid = fs_uuid_unsupported,
		.opendir = fat_opendir,
		.readdir = fat_readdir,
		.closedir = fat_closedir,
	},
#endif
#ifdef CONFIG_FS_EXT4
	{
		.fstype = FS_TYPE_EXT,
		.name = "ext4",
		.null_dev_desc_ok = false,
		.probe = ext4fs_probe,
		.close = ext4fs_close,
		.ls = ext4fs_ls,
		.exists = ext4fs_exists,
		.size = ext4fs_size,
		.read = ext4_read_file,
#ifdef CONFIG_CMD_EXT4_WRITE
		.write = ext4_write_file,
#else
		.write = fs_write_unsupported,
#endif
		.uuid = ext4fs_uuid,
		.opendir = fs_opendir_unsupported,
	},
#endif
#ifdef CONFIG_SANDBOX
	{
		.fstype = FS_TYPE_SANDBOX,
		.name = "sandbox",
		.null_dev_desc_ok = true,
		.probe = sandbox_fs_set_blk_dev,
		.close = sandbox_fs_close,
		.ls = sandbox_fs_ls,
		.exists = sandbox_fs_exists,
		.size = sandbox_fs_size,
		.read = fs_read_sandbox,
		.write = fs_write_sandbox,
		.uuid = fs_uuid_unsupported,
		.opendir = fs_opendir_unsupported,
	},
#endif
#ifdef CONFIG_CMD_UBIFS
	{
		.fstype = FS_TYPE_UBIFS,
		.name = "ubifs",
		.null_dev_desc_ok = true,
		.probe = ubifs_set_blk_dev,
		.close = ubifs_close,
		.ls = ubifs_ls,
		.exists = ubifs_exists,
		.size = ubifs_size,
		.read = ubifs_read,
		.write = fs_write_unsupported,
		.uuid = fs_uuid_unsupported,
		.opendir = fs_opendir_unsupported,
	},
#endif
	{
		.fstype = FS_TYPE_ANY,
		.name = "unsupported",
		.null_dev_desc_ok = true,
		.probe = fs_probe_unsupported,
		.close = fs_close_unsupported,
		.ls = fs_ls_unsupported,
		.exists = fs_exists_unsupported,
		.size = fs_size_unsupported,
		.read = fs_read_unsupported,
		.write = fs_write_unsupported,
		.uuid = fs_uuid_unsupported,
		.opendir = fs_opendir_unsupported,
	},
};

static struct fstype_info *fs_get_info(int fstype)
{
	struct fstype_info *info;
	int i;

	for (i = 0, info = fstypes; i < ARRAY_SIZE(fstypes) - 1; i++, info++) {
		if (fstype == info->fstype)
			return info;
	}

	/* Return the 'unsupported' sentinel */
	return info;
}

int fs_set_blk_dev(const char *ifname, const char *dev_part_str, int fstype)
{
	struct fstype_info *info;
	int part, i;
#ifdef CONFIG_NEEDS_MANUAL_RELOC
	static int relocated;

	if (!relocated) {
		for (i = 0, info = fstypes; i < ARRAY_SIZE(fstypes);
				i++, info++) {
			info->name += gd->reloc_off;
			info->probe += gd->reloc_off;
			info->close += gd->reloc_off;
			info->ls += gd->reloc_off;
			info->read += gd->reloc_off;
			info->write += gd->reloc_off;
		}
		relocated = 1;
	}
#endif

	part = blk_get_device_part_str(ifname, dev_part_str, &fs_dev_desc,
					&fs_partition, 1);
	if (part < 0)
		return -1;

	for (i = 0, info = fstypes; i < ARRAY_SIZE(fstypes); i++, info++) {
		if (fstype != FS_TYPE_ANY && info->fstype != FS_TYPE_ANY &&
				fstype != info->fstype)
			continue;

		if (!fs_dev_desc && !info->null_dev_desc_ok)
			continue;

		if (!info->probe(fs_dev_desc, &fs_partition)) {
			fs_type = info->fstype;
			fs_dev_part = part;
			return 0;
		}
	}

	return -1;
}

/* set current blk device w/ blk_desc + partition # */
int fs_set_blk_dev_with_part(struct blk_desc *desc, int part)
{
	struct fstype_info *info;
	int ret, i;

	if (part >= 1)
		ret = part_get_info(desc, part, &fs_partition);
	else
		ret = part_get_info_whole_disk(desc, &fs_partition);
	if (ret)
		return ret;
	fs_dev_desc = desc;

	for (i = 0, info = fstypes; i < ARRAY_SIZE(fstypes); i++, info++) {
		if (!info->probe(fs_dev_desc, &fs_partition)) {
			fs_type = info->fstype;
			return 0;
		}
	}

	return -1;
}

int fs_get_fstype(const char **fstype_name)
{
	struct fstype_info *info;

	if (fstype_name == NULL) {
		printf("** parameter error **\n");
		return -1;
	}

	info = fs_get_info(fs_type);
	if (info->fstype == FS_TYPE_ANY) {
		printf("** not match any filesystem type **\n");
		return -1;
	}

	*fstype_name = info->name;
	return 0;
}

static void fs_close(void)
{
	struct fstype_info *info = fs_get_info(fs_type);

	info->close();

	fs_type = FS_TYPE_ANY;
}

int fs_uuid(char *uuid_str)
{
	struct fstype_info *info = fs_get_info(fs_type);

	return info->uuid(uuid_str);
}

int fs_ls(const char *dirname)
{
	int ret;

	struct fstype_info *info = fs_get_info(fs_type);

	ret = info->ls(dirname);

	fs_type = FS_TYPE_ANY;
	fs_close();

	return ret;
}

int fs_exists(const char *filename)
{
	int ret;

	struct fstype_info *info = fs_get_info(fs_type);

	ret = info->exists(filename);

	fs_close();

	return ret;
}

int fs_size(const char *filename, loff_t *size)
{
	int ret;

	struct fstype_info *info = fs_get_info(fs_type);

	ret = info->size(filename, size);

	fs_close();

	return ret;
}


void extract_signature(uint8_t *image, size_t image_size, uint8_t *signature) {
	memcpy(signature, image + (image_size - SIGNATURE_SIZE), SIGNATURE_SIZE);
 }


//  int verify_signature(const uint8_t *signature, size_t sig_len, 
// 	const uint8_t *hash, size_t hash_len,
// 	const struct key_prop *pubkey) {
// int ret = rsa_verify(pubkey, signature, sig_len, hash, hash_len);
// return ret == 0;  // 0 means success
// }

int fs_read(const char *filename, ulong addr, loff_t offset, loff_t len,
	    loff_t *actread)
{
	struct fstype_info *info = fs_get_info(fs_type);
	void *buf;
	int ret;
	//int iRet=0;
	char image_bytes[4]={0};
	//char *efuse_data=NULL;
	uint8_t kernel_hash[32]={0};
	// uint8_t signature[SIGNATURE_SIZE];
	// struct key_prop pubkey;

	/*
	 * We don't actually know how many bytes are being read, since len==0
	 * means read the whole file.
	 */

	buf = map_sysmem(addr, len);
	ret = info->read(filename, buf, offset, len, actread);

	printf("file name is %s length is %ld\n",filename,strlen(kernel_image));

	if(strcmp(filename,kernel_image)==0 ){
			printf("<->-<->-<->-<->- Entered the comparsion space -<->-<->-<->-<->\n");
			//fs_read_into_buff(filename,image_buffer,addr,pos,bytes,&len_read);
			printf("actread value is %lld\n",(*actread));
			// if(buf!=NULL){
			// 	for(int i=3;i>=0;i--){
			// 		if(((char*)(buf))[(*actread)-1-i]=='\0'){
			// 			printf("0x0000 0000\n");
						
			// 		}
			// 		else{
			// 			printf("%c\n",((char* )(buf))[(*actread)-1-i]);
			// 		}
			// 		image_bytes[i]=((char*)(buf))[(*actread)-1-i];
			// 	}
			// }
			
			// //unmap_sysmem(image_buffer);

			//compute_sha256(buf,(*actread),kernel_hash);
			for(int i=0;i<32;i++){
				printf("hash[%d] is %d\n",i,kernel_hash[i]);
			}
			//extract_signature(buf,(*actread),signature);

			// iRet=read_public_key(efuse_data);
			
			
			// //printf("just printing iRet %d\n",iRet);
			// if(iRet==-EINVAL){
			// 	printf("couldn' read public key fail.............\n");
			// }
			// else{
			// 	printf("public key read success fully\n");
			// }
			

			// for(int i=0;i<256;i++){
			// 	pubkey.key[i]=(uint8_t*)efuse_data[i];
			// }
			// if(verify_signature(signature,SIGNATURE_SIZE,kernel_hash,32,pubkey)){
			// 	printf(" Kernel Verified - Booting\n");
			// 	return 0;
			// }
			// else{
			// 	printf(" Kernel Verification Failed - Aborting\n");
        		// 	return 1;
			// }

			for(int i=0;i<4;i++){

				// if(efuse_data[i]==image_bytes[i]){
				// 	printf("index %d matched\n",i);
				// }
				// else{
				// 	printf("index %d unmatched\n",i);
				// }
				if(image_bytes[i]=='\0'){
					printf("0x0000 0000\n");
				}
				else{
					printf("%c\n",image_bytes[i]);
				}
			}
	}
	unmap_sysmem(buf);

	/* If we requested a specific number of bytes, check we got it */
	if (ret == 0 && len && *actread != len)
		printf("** %s shorter than offset + len **\n", filename);
	fs_close();

	return ret;
}

int fs_write(const char *filename, ulong addr, loff_t offset, loff_t len,loff_t *actwrite)
{
	struct fstype_info *info = fs_get_info(fs_type);
	void *buf;
	int ret;

	buf = map_sysmem(addr, len);
	ret = info->write(filename, buf, offset, len, actwrite);
	unmap_sysmem(buf);

	if (ret < 0 && len != *actwrite) {
		printf("** Unable to write file %s **\n", filename);
		ret = -1;
	}
	fs_close();

	return ret;
}

struct fs_dir_stream *fs_opendir(const char *filename)
{
	struct fstype_info *info = fs_get_info(fs_type);
	struct fs_dir_stream *dirs = NULL;
	int ret;

	ret = info->opendir(filename, &dirs);
	fs_close();
	if (ret) {
		errno = -ret;
		return NULL;
	}

	dirs->desc = fs_dev_desc;
	dirs->part = fs_dev_part;

	return dirs;
}

struct fs_dirent *fs_readdir(struct fs_dir_stream *dirs)
{
	struct fstype_info *info;
	struct fs_dirent *dirent;
	int ret;

	fs_set_blk_dev_with_part(dirs->desc, dirs->part);
	info = fs_get_info(fs_type);

	ret = info->readdir(dirs, &dirent);
	fs_close();
	if (ret) {
		errno = -ret;
		return NULL;
	}

	return dirent;
}

void fs_closedir(struct fs_dir_stream *dirs)
{
	struct fstype_info *info;

	if (!dirs)
		return;

	fs_set_blk_dev_with_part(dirs->desc, dirs->part);
	info = fs_get_info(fs_type);

	info->closedir(dirs);
	fs_close();
}


int do_size(cmd_tbl_t *cmdtp, int flag, int argc, char * const argv[],int fstype)
{
	loff_t size;

	if (argc != 4)
		return CMD_RET_USAGE;

	if (fs_set_blk_dev(argv[1], argv[2], fstype))
		return 1;

	if (fs_size(argv[3], &size) < 0)
		return CMD_RET_FAILURE;

	env_set_hex("filesize", size);

	return 0;
}


int read_public_key(char* ptr){

	struct udevice* dev;
	int ret,offset=0;
	ptr=buffer;
	int* iptr=(int*)buffer;
	ret = uclass_get_device_by_driver(UCLASS_MISC,
					  DM_GET_DRIVER(rockchip_efuse), &dev);
	if (ret) {
		printf("%s: no misc-device found\n", __func__);
		return -EINVAL;
	}
	
	ret=boot_efuse_read(dev,offset,buffer,sizeof(buffer));

	if(ret)
	printf("reading efuse-failed miserably\n");

	// puts("printing efuse buffer...........................\n");
	// for(int i=0;i<sizeof(buffer);i++){
	// 	if(buffer[i]=='\0'){
	// 		puts("\0x00000000\t");
	// 	}
	// 	else{
	// 		printf("%c\t",buffer[i]);
	// 	}
	// 	if(i%2!=0){
	// 		puts("\n");
	// 	}
	// }

	puts("---------------(integer formatted):::::::::printing efuse buffer::::::::::::\n");
	// for(int i=0;i<sizeof(buffer)/sizeof(int);i++){
	// 	printf("index: %d , data:  0x%x \n",i,iptr[i]);
	// }


    for(int i=0;i<sizeof(buffer)/sizeof(int);i++){
        printf("i: %d, buffer[i]: 0x%x\n",i,iptr[i]);
        puts("\n");
		char* cptr=(char*)(iptr+i);
        printf("[\t");
        for(int j=0;j<sizeof(int);j++){
            printf("{ j: %d, char[j]: %d }\t",j,cptr[j]);
        }
        puts("]\n");
		puts("\n");
    }


	puts("---------------(integer formatted):::::::::printing efuse buffer::::::::::::\n");
	for(int i=0;i<sizeof(integer_array)/sizeof(u32);i++){
		printf("[ i: %d, integer_array[i]: %u , integer_array[i]: %x\n",i,integer_array[i],integer_array[i]);

	}


	
	
	return 0;
	
}




int write_public_key(char* ptr){

	struct udevice* dev;
	int ret,offset=0;
	//int i=1;
	ptr=buffer;
	ret = uclass_get_device_by_driver(UCLASS_MISC,
					  DM_GET_DRIVER(rockchip_efuse), &dev);
	if (ret) {
		printf("%s: no misc-device found\n", __func__);
		return -EINVAL;
	}
	
	
	memset(buffer, 'a', sizeof(buffer));
	ret=boot_efuse_write(dev,offset,buffer,sizeof(buffer));

	
	
	return 0;
	
}


static int boot_efuse_read(struct udevice *dev, int offset,void *buf, int size){
	struct rockchip_efuse_platdata *plat = dev_get_platdata(dev);
 
	if (!plat) {
	    printf("boot_efuse_read: plat is NULL, read failed\n");
	    return -EINVAL;
	}
 
	struct rockchip_efuse_regs *efuse = (struct rockchip_efuse_regs *)plat->base;
	unsigned int addr_start, addr_end, addr_offset;
	u32 out_value;
	u8  bytes[RK3399_NFUSES * RK3399_BYTES_PER_FUSE];
	
	//int i = 0;
	u32 addr=0;	
	//u32 trial_value=0;

	addr_start = offset / RK3399_BYTES_PER_FUSE;
	addr_offset = offset % RK3399_BYTES_PER_FUSE;
	addr_end = DIV_ROUND_UP(offset + size, RK3399_BYTES_PER_FUSE);
 
	if (addr_end > RK3399_NFUSES)
	addr_end = RK3399_NFUSES;

	printf("addr start: %u\n,addr offset: %u,addr end: %u\n",addr_start,addr_offset,addr_end);

	writel( RK3399_LOAD | RK3399_PGENB | RK3399_STROBSFTSEL | RK3399_RSB ,
	&efuse->ctrl);

	printf("current status during efuse read is 0x%08x\n",efuse->ctrl);

	udelay(1);
	for (addr = addr_start; addr < 32; addr++) {
		
		setbits_le32(&efuse->ctrl,
		RK3399_STROBE | (addr << RK3399_A_SHIFT));
		/*
		
			in efuse control register, addresses can be provided in the range 
			0 to 1023 i.e from address pins A0 to A9 since 2^10=1024
			And in the controller register start from bit [16 - 25]
			hence it is done addr << RK3399_A_SHIFT, where RK3399_A_SHIFT = 16
		
		*/
		udelay(1);
		out_value = readl(&efuse->dout);
		//if(addr==8){
			//trial_value=readl(&efuse->ctrl+addr);

			
		//}
		
		clrbits_le32(&efuse->ctrl, RK3399_STROBE);
		udelay(1);
		//printf("[ addr = %d, trial value is 0x%x ]\n",addr,trial_value);
		printf("[ addr = %d, out_value = 0x%x ]\n ",addr,out_value);
		memcpy(&bytes[addr*sizeof(u32)], &out_value, RK3399_BYTES_PER_FUSE);
		memcpy(&integer_array[addr],&out_value,sizeof(u32));
	}
 
	/* Switch to standby mode */
	writel(RK3399_PD | RK3399_CSB, &efuse->ctrl);
 
	if (!buf) {
	    printf("Error: buf is NULL, cannot copy data!\n");
	    return -EINVAL;
	}
 
	memcpy(buf, bytes + addr_offset, size);
	return 0;
}


// static ulong rk3399_saradc_get_clk(struct rk3399_cru *cru)
// {
// 	u32 div, val;

// 	val = readl(&cru->clksel_con[26]);
// 	div = bitfield_extract(val, CLK_SARADC_DIV_CON_SHIFT,
// 			       CLK_SARADC_DIV_CON_WIDTH);

// 	return DIV_TO_RATE(OSC_HZ, div);
// }


// static ulong rk3399_saradc_set_clk(struct rk3399_cru *cru, uint hz)
// {
// 	int src_clk_div;

// 	src_clk_div = DIV_ROUND_UP(OSC_HZ, hz) - 1;
// 	assert(src_clk_div <= 255);

// 	rk_clrsetreg(&cru->clksel_con[26],
// 		     CLK_SARADC_DIV_CON_MASK,
// 		     src_clk_div << CLK_SARADC_DIV_CON_SHIFT);

// 	return rk3399_saradc_get_clk(cru);
// }


static int boot_efuse_write(struct udevice *dev, int offset,void *buf, int size){
	struct rockchip_efuse_platdata *plat = dev_get_platdata(dev);
 
	if (!plat) {
	    printf("boot_efuse_read: plat is NULL, read failed\n");
	    return -EINVAL;
	}
 
	struct rockchip_efuse_regs *efuse = (struct rockchip_efuse_regs *)plat->base;
	unsigned int addr_start, addr_end, addr_offset;
	
	//value trid earlier , 1) 78, 2) 75 , 3)INT_MAX 4) 0 : 11, 0: 12 
	//5) 35: 13, 35: 14 , 35 : 15 
	// 6) 0xFFFFFFFF : 15, 16 ,7) 0xFF : 20
	u8 out_value=0xFF;

	u32 addr=0;

	addr_start = offset / RK3399_BYTES_PER_FUSE;
	addr_offset = offset % RK3399_BYTES_PER_FUSE;
	addr_end = DIV_ROUND_UP(offset + size, RK3399_BYTES_PER_FUSE);
 
	if (addr_end > RK3399_NFUSES)
	addr_end = RK3399_NFUSES;

	printf("addr start: %u\n,addr offset: %u,addr end: %u\n",addr_start,addr_offset,addr_end);

	writel( RK3399_PS | RK3399_STROBSFTSEL | RK3399_RSB ,
	&efuse->ctrl);

	printf("current status during efuse read is 0x%08x\n",(efuse->ctrl));
	//efuse->strobe_finish_ctrl=0;
	addr=19;
	// addr tried -> 1) 30, 2) 10  3) 10 4) 11 5) 12 6) 13 7) 14 8) 15 9) 18 10) 20
	struct rk3399_cru* cru=rockchip_get_cru();
	
	if(cru==NULL){
		printf("cru is NULL\n");
	}	

	ulong cru_clk=rk3399_saradc_get_clk(cru);
	printf("the cru is %lu \n",cru_clk);
	
	for(int i=0;i<32;i++){
		setbits_le32(&efuse->ctrl,
		RK3399_STROBE | (addr << RK3399_A_SHIFT << i));
		udelay(13);
		//writeb(out_value,(volatile unsigned char*)&efuse->dout2);
		//udelay(20);
		clrbits_le32(&efuse->ctrl, RK3399_STROBE);
		//udelay(20);
		printf("[ addr = %d, out_value = 0x%x \n ",addr,out_value);
 
	}
	/* Switch to standby mode */
	writel(RK3399_PD | RK3399_CSB, &efuse->ctrl);
	udelay(10);
	if (!buf) {
	    printf("Error: buf is NULL, cannot copy data!\n");
	    return -EINVAL;
	}
 
	
	return 0;
}




// static int boot_efuse_write(struct udevice *dev, int offset,void *buf, int size){
// 	struct rockchip_efuse_platdata *plat = dev_get_platdata(dev);
 
// 	if (!plat) {
// 	    printf("boot_efuse_read: plat is NULL, read failed\n");
// 	    return -EINVAL;
// 	}
 
// 	struct rockchip_efuse_regs *efuse = (struct rockchip_efuse_regs *)plat->base;
// 	unsigned int addr_start, addr_end, addr_offset;
	
// 	//value trid earlier , 1) 78, 2) 75 , 3)INT_MAX 4) 0 : 11, 0: 12 
// 	//5) 35: 13, 35: 14 , 35 : 15 
// 	// 6) 0xFFFFFFFF : 15, 16
// 	u32 out_value=0xFFFFFFFF;

// 	//u8  bytes[RK3399_NFUSES * RK3399_BYTES_PER_FUSE];
	
// 	//int i = 0;
// 	u32 addr=0;

// 	addr_start = offset / RK3399_BYTES_PER_FUSE;
// 	addr_offset = offset % RK3399_BYTES_PER_FUSE;
// 	addr_end = DIV_ROUND_UP(offset + size, RK3399_BYTES_PER_FUSE);
 
// 	if (addr_end > RK3399_NFUSES)
// 	addr_end = RK3399_NFUSES;

// 	printf("addr start: %u\n,addr offset: %u,addr end: %u\n",addr_start,addr_offset,addr_end);

// 	writel( RK3399_PS | RK3399_STROBSFTSEL | RK3399_RSB ,
// 	&efuse->ctrl);

// 	printf("current status during efuse read is 0x%08x\n",efuse->ctrl);

// 	addr=20;
// 	// addr tried -> 1) 30, 2) 10  3) 10 4) 11 5) 12 6) 13 7) 14 8) 15

// 	udelay(15);
// //	for (addr = addr_start; addr < 32; addr++) {

// 		setbits_le32(&efuse->ctrl,
// 		RK3399_STROBE | (addr << RK3399_A_SHIFT));
// 		/*
		
// 			in efuse control register, addresses can be provided in the range 
// 			0 to 1023 i.e from address pins A0 to A9 since 2^10=1024
// 			And in the controller register start from bit [16 - 25]
// 			hence it is done addr << RK3399_A_SHIFT, where RK3399_A_SHIFT = 16
		
// 		*/
// 		udelay(20);
// 		writel(out_value,&efuse->dout2);
// 		udelay(20);
// 		clrbits_le32(&efuse->ctrl, RK3399_STROBE);
// 		udelay(20);

// 		printf("[ addr = %d, out_value = 0x%x \n ",addr,out_value);
// //		memcpy(&bytes[addr*sizeof(u32)], &out_value, RK3399_BYTES_PER_FUSE);
// //		memcpy(&integer_array[addr],&out_value,sizeof(u32));
// //	}
 
// 	/* Switch to standby mode */
// 	writel(RK3399_PD | RK3399_CSB, &efuse->ctrl);
// 	udelay(10);
// 	if (!buf) {
// 	    printf("Error: buf is NULL, cannot copy data!\n");
// 	    return -EINVAL;
// 	}
 
// 	//memcpy(buf, bytes + addr_offset, size);
// 	return 0;
// }



// static int boot_efuse_write(struct udevice *dev, int offset,void *buf, int size){
// 	struct rockchip_efuse_platdata *plat = dev_get_platdata(dev);
 
// 	if (!plat) {
// 	    printf("boot_efuse_read: plat is NULL, read failed\n");
// 	    return -EINVAL;
// 	}
 
// 	struct rockchip_efuse_regs *efuse = (struct rockchip_efuse_regs *)plat->base;
// 	unsigned int addr_start, addr_end, addr_offset;
	
// 	//value trid earlier , 1) 78, 2) 75 , 3)INT_MAX 4) 0 : 11, 0: 12 
// 	//5) 35: 13, 35: 14 , 35 : 15 
// 	// 6) 0xFFFFFFFF : 15, 16
// 	u32 out_value=0xFFFFFFFF;

// 	//u8  bytes[RK3399_NFUSES * RK3399_BYTES_PER_FUSE];
	
// 	//int i = 0;
// 	u32 addr=0;

// 	addr_start = offset / RK3399_BYTES_PER_FUSE;
// 	addr_offset = offset % RK3399_BYTES_PER_FUSE;
// 	addr_end = DIV_ROUND_UP(offset + size, RK3399_BYTES_PER_FUSE);
 
// 	if (addr_end > RK3399_NFUSES)
// 	addr_end = RK3399_NFUSES;

// 	printf("addr start: %u\n,addr offset: %u,addr end: %u\n",addr_start,addr_offset,addr_end);

// 	writel( RK3399_PS | RK3399_STROBSFTSEL | RK3399_RSB ,
// 	&efuse->ctrl);

// 	printf("current status during efuse read is 0x%08x\n",efuse->ctrl);

// 	addr=15;
// 	// addr tried -> 1) 30, 2) 10  3) 10 4) 11 5) 12 6) 13 7) 14 8) 15

// 	udelay(15);
// //	for (addr = addr_start; addr < 32; addr++) {

// 		setbits_le32(&efuse->ctrl,
// 		RK3399_STROBE | (addr << RK3399_A_SHIFT));
// 		/*
		
// 			in efuse control register, addresses can be provided in the range 
// 			0 to 1023 i.e from address pins A0 to A9 since 2^10=1024
// 			And in the controller register start from bit [16 - 25]
// 			hence it is done addr << RK3399_A_SHIFT, where RK3399_A_SHIFT = 16
		
// 		*/
// 		udelay(20);
// 		writel(out_value,&efuse->dout2);
// 		udelay(20);
// 		clrbits_le32(&efuse->ctrl, RK3399_STROBE);
// 		udelay(20);

// 		printf("[ addr = %d, out_value = 0x%x \n ",addr,out_value);
// //		memcpy(&bytes[addr*sizeof(u32)], &out_value, RK3399_BYTES_PER_FUSE);
// //		memcpy(&integer_array[addr],&out_value,sizeof(u32));
// //	}
 
// 	/* Switch to standby mode */
// 	writel(RK3399_PD | RK3399_CSB, &efuse->ctrl);
// 	udelay(10);
// 	if (!buf) {
// 	    printf("Error: buf is NULL, cannot copy data!\n");
// 	    return -EINVAL;
// 	}
 
// 	//memcpy(buf, bytes + addr_offset, size);
// 	return 0;
// }




int do_load(cmd_tbl_t *cmdtp, int flag, int argc, char * const argv[],
		int fstype)
{
	unsigned long addr;
	const char *addr_str;
	const char *filename;
	loff_t bytes;
	loff_t pos;
	loff_t len_read;
	int ret;
	int iRet=0;
	char image_bytes[4]={0};
	char *efuse_data=NULL;
	unsigned long time;
	//void* image_buffer=NULL;
	char *ep;
	
	if (argc < 2)
		return CMD_RET_USAGE;
	if (argc > 7)
		return CMD_RET_USAGE;

	if (fs_set_blk_dev(argv[1], (argc >= 3) ? argv[2] : NULL, fstype))
		return 1;

	if (argc >= 4) {
		addr = simple_strtoul(argv[3], &ep, 16);
		if (ep == argv[3] || *ep != '\0')
			return CMD_RET_USAGE;
	} else {
		addr_str = env_get("loadaddr");
		if (addr_str != NULL)
			addr = simple_strtoul(addr_str, NULL, 16);
		else
			addr = CONFIG_SYS_LOAD_ADDR;
	}
	if (argc >= 5) {
		filename = argv[4];
	} else {
		filename = env_get("bootfile");
		if (!filename) {
			puts("** No boot file defined **\n");
			return 1;
		}
	}
	
	printf("manual: in <file fs.c ,function do_load> filename is %s of length %ld\n",filename,strlen(filename));
	puts("\n\n");
	if (argc >= 6)
		bytes = simple_strtoul(argv[5], NULL, 16);
	else
		bytes = 0;
	if (argc >= 7)
		pos = simple_strtoul(argv[6], NULL, 16);
	else
		pos = 0;

	time = get_timer(0);

	if(strcmp(filename,kernel_image)==0 ){
			printf("<->-<->-<->-<->- Entered the comparsion space -<->-<->-<->-<->\n");
			// fs_read_into_buff(filename,image_buffer,addr,pos,bytes,&len_read);
			// if(image_buffer!=NULL){
			// 	for(int i=0;i<=3;i++){
			// 		if(((char*)(image_buffer))[len_read-1-i]=='\0'){
			// 			printf("0x0000 0000\n");
						
			// 		}
			// 		else{
			// 			printf("%c\n",((char* )(image_buffer))[len_read-1-i]);
			// 		}
			// 		image_bytes[i]=((char*)(image_buffer))[len_read-1-i];
			// 	}
			// }
			
			// unmap_sysmem(image_buffer);
	

			printf("1st process of reading -------------------------------------------\n");
			puts("~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~\n");
			iRet=read_public_key(efuse_data);
			if(iRet==-EINVAL){
				printf("couldn' read public key fail.............\n");
			}
			else{
				printf("public key read success fully\n");
			}

			printf("2nd proces of writing -------------------------------------------\n");
			puts("~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~\n");
			
			iRet=write_public_key(efuse_data);
			if(iRet==-EINVAL){
				printf("couldn' write public key fail.............\n");
			}
			else{
				printf("public key write success fully\n");
			}

			printf("3rd process of reading -------------------------------------------\n");
			puts("~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~\n");
			
			iRet=read_public_key(efuse_data);
			if(iRet==-EINVAL){
				printf("couldn' read public key fail.............\n");
			}
			else{
				printf("public key read success fully\n");
			}
			

			for(int i=0;i<4;i++){

				// if(efuse_data[i]==image_bytes[i]){
				// 	printf("index %d matched\n",i);
				// }
				// else{
				// 	printf("index %d unmatched\n",i);
				// }
				if(image_bytes[i]=='\0'){
					printf("0x0000 0000\n");
				}
				else{
					printf("%c\n",image_bytes[i]);
				}
			}
	}
	
	ret = fs_read(filename, addr, pos, bytes, &len_read);
	time = get_timer(time);
	if (ret < 0)
		return 1;

	printf("%llu bytes read in %lu ms", len_read, time);

	printf("manual: in <file fs.c ,function do_load> addr is %lu\n",addr);
	printf("manual: in <file fs.c ,function do_load> pos is %lld\n",pos);
	printf("manual: in <file fs.c ,function do_load> bytes  is %llu\n",bytes);
	printf("manual: in <file fs.c ,function do_load> len_read is %lld\n",len_read);
	if (time > 0) {
		puts(" (");
		print_size(div_u64(len_read, time) * 1000, "/s");
		puts(")");
	}
	puts("\n");

	env_set_hex("fileaddr", addr);
	env_set_hex("filesize", len_read);

	return 0;
}

int do_ls(cmd_tbl_t *cmdtp, int flag, int argc, char * const argv[],
	int fstype)
{
	if (argc < 2)
		return CMD_RET_USAGE;
	if (argc > 4)
		return CMD_RET_USAGE;

	if (fs_set_blk_dev(argv[1], (argc >= 3) ? argv[2] : NULL, fstype))
		return 1;

	if (fs_ls(argc >= 4 ? argv[3] : "/"))
		return 1;

	return 0;
}

int file_exists(const char *dev_type, const char *dev_part, const char *file,
		int fstype)
{
	if (fs_set_blk_dev(dev_type, dev_part, fstype))
		return 0;

	return fs_exists(file);
}

int do_save(cmd_tbl_t *cmdtp, int flag, int argc, char * const argv[],
		int fstype)
{
	unsigned long addr;
	const char *filename;
	loff_t bytes;
	loff_t pos;
	loff_t len;
	int ret;
	
	unsigned long time;

	if (argc < 6 || argc > 7)
		return CMD_RET_USAGE;

	if (fs_set_blk_dev(argv[1], argv[2], fstype))
		return 1;

	addr = simple_strtoul(argv[3], NULL, 16);
	filename = argv[4];
	bytes = simple_strtoul(argv[5], NULL, 16);
	if (argc >= 7)
		pos = simple_strtoul(argv[6], NULL, 16);
	else
		pos = 0;

	time = get_timer(0);
	ret = fs_write(filename, addr, pos, bytes, &len);
	time = get_timer(time);
	if (ret < 0)
		return 1;

	printf("%llu bytes written in %lu ms", len, time);
	if (time > 0) {
		puts(" (");
		print_size(div_u64(len, time) * 1000, "/s");
		puts(")");
	}
	puts("\n");

	return 0;
}

int do_fs_uuid(cmd_tbl_t *cmdtp, int flag, int argc, char * const argv[],
		int fstype)
{
	int ret;
	char uuid[37];
	memset(uuid, 0, sizeof(uuid));

	if (argc < 3 || argc > 4)
		return CMD_RET_USAGE;

	if (fs_set_blk_dev(argv[1], argv[2], fstype))
		return 1;

	ret = fs_uuid(uuid);
	if (ret)
		return CMD_RET_FAILURE;

	if (argc == 4)
		env_set(argv[3], uuid);
	else
		printf("%s\n", uuid);

	return CMD_RET_SUCCESS;
}

int do_fs_type(cmd_tbl_t *cmdtp, int flag, int argc, char * const argv[])
{
	struct fstype_info *info;

	if (argc < 3 || argc > 4)
		return CMD_RET_USAGE;

	if (fs_set_blk_dev(argv[1], argv[2], FS_TYPE_ANY))
		return 1;

	info = fs_get_info(fs_type);

	if (argc == 4)
		env_set(argv[3], info->name);
	else
		printf("%s\n", info->name);

	return CMD_RET_SUCCESS;
}

