#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "log.h"
#include "idevicedfu.h"
#include "ideviceloaders.h"

#include "gastera1n.h"
#include "kernel64patcher.h"

#include <plist/plist.h>
#include <libfragmentzip/libfragmentzip.h>


static device_loader ipsw_loader;
static char *ipsw_url;
static const char *rdsk_staging_path = "/tmp/gastera1n_rdsk",\
*rdsk_mount_path = "/tmp/gastera1n_rdsk/dmg_mountpoint",\
*kernelcache_save_path = "/tmp/gastera1n_rdsk/kernelcache.release",\
*trustcache_save_path = "/tmp/gastera1n_rdsk/dmg.trustcache",\
*ramdisk_save_path = "/tmp/gastera1n_rdsk/ramdisk.dmg",\
*devicetree_save_path = "/tmp/gastera1n_rdsk/DeviceTree.im4p",\
*iBEC_save_path = "/tmp/gastera1n_rdsk/iBEC.im4p",\
*iBSS_save_path = "/tmp/gastera1n_rdsk/iBSS.im4p";
static const char *iBSS_img4_path = "/tmp/gastera1n_rdsk/ibss.img4",\
*iBEC_img4_path = "/tmp/gastera1n_rdsk/ibec.img4",\
*bootim_img4_path = "/tmp/gastera1n_rdsk/bootim.img4",\
*devicetree_img4_path = "/tmp/gastera1n_rdsk/dtree.img4",\
*ramdisk_img4_path = "/tmp/gastera1n_rdsk/rdsk.img4",\
*trustcache_img4_path = "/tmp/gastera1n_rdsk/trustcache.img4",\
*kernelcache_img4_path = "/tmp/gastera1n_rdsk/kernelcache.img4";

static const char *ldid2 = "ldid2";
static const char *tsschecker = "tsschecker_macOS_v440";

void
im4m_from_shsh(char *path, char *im4m_path) {
    FILE *f = fopen(path, "r");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    size_t size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* data = (char*)malloc(size);
    if (data) fread(data, size, 1, f);
    fclose(f);
    plist_t shsh_plist = NULL;
    plist_from_memory((const char*)data, (uint32_t)size, &shsh_plist, NULL);
    plist_t ticket = plist_dict_get_item(shsh_plist, "ApImg4Ticket");
    char *im4m = 0;
    uint64_t im4m_size = 0;
    plist_get_data_val(ticket, &im4m, &im4m_size);
    f = fopen(im4m_path, "wb");
    fwrite(im4m, 1, im4m_size, f);
    fclose(f);
}

// kerneldiff original source: https://raw.githubusercontent.com/verygenericname/kerneldiff_C/refs/heads/main/kerneldiff.c
#define MAX_DIFF 16384
#define DIFF_COMP_MAX_SIZE 20

static int
kerneldiff(char *kc_raw, char *kc_patched, char *kc_diff) {

  struct stat st2;
  stat(kc_patched, &st2);
  FILE *fp2 = fopen(kc_patched, "rb");
  char *p = malloc(st2.st_size);
  fread(p, 1, st2.st_size, fp2);
  fclose(fp2);

  struct stat st1;
  stat(kc_raw, &st1);
  FILE *fp1 = fopen(kc_raw, "rb");
  char *o = malloc(st1.st_size);
  fread(o, 1, st1.st_size, fp1);
  fclose(fp1);

  char diff[MAX_DIFF][3][DIFF_COMP_MAX_SIZE];
  int diffIndex = 0;

  for (int i = 0; i < st1.st_size; i++) {
    char originalByte = o[i];
    char patchedByte = p[i];

    if (originalByte != patchedByte) {
     if ((diffIndex + 1) > MAX_DIFF) {
        fprintf(stderr, "kerneldiff: too many differences, only a maximum %d 8-byte differences are supported\n", MAX_DIFF);
        return -1;
      }
      snprintf(diff[diffIndex][0], DIFF_COMP_MAX_SIZE, "0x%x", i);

      snprintf(diff[diffIndex][1], DIFF_COMP_MAX_SIZE, "0x%x", originalByte);

      snprintf(diff[diffIndex][2], DIFF_COMP_MAX_SIZE, "0x%x", patchedByte);

      diffIndex++;
    }
  }

  free(p);
  free(o);

  FILE *fp0 = fopen(kc_diff, "w+");
  fwrite("#AMFI\n\n", 1, 7, fp0);

  for (int i = 0; i < diffIndex; i++) {
    int dataSize = strlen(diff[i][0]) + strlen(diff[i][1]) + strlen(diff[i][2]) + 3;
    char data[dataSize];
    sprintf(data, "%s %s %s\n", diff[i][0], diff[i][1], diff[i][2]);

    fwrite(data, 1, dataSize, fp0);
    printf("%s", data);
  }

  fclose(fp0);

  return 0;
}

static bool
execute_command(char *command) {
	log_debug("Executing command: %s\n", command);
	FILE *fp;
	fp = popen(command, "r");

	if (fp == NULL) {
		return false;
	}

    size_t line_sz = 130;
	char line[line_sz];

	while (fgets( line, line_sz, fp)){
	  printf("%s", line);
	}

	pclose(fp);
	return true;
}

static void
default_prog_cb(unsigned int progress) {

	if(progress < 0) {
		return;
	}

	if(progress > 100) {
		progress = 100;
	}

	printf("\r[");

	for(unsigned int i = 0; i < 50; i++) {
		if(i < progress / 2) {
			printf("=");
		} else {
			printf(" ");
		}
	}

	printf("] %3.1d%%", progress);

	fflush(stdout);

	if(progress == 100) {
		printf("\n");
	}
}

static int
download_firmware_component(const char *component_path, const char *out_path) {
	int ret;
	fragmentzip_t *ipsw = fragmentzip_open(ipsw_url);

	if (!ipsw) {
		return -1;
	}

	log_debug("Downloading %s, saving as: %s\n", component_path, out_path);

	ret = fragmentzip_download_file(ipsw, component_path, out_path, default_prog_cb);
	fragmentzip_close(ipsw);

	if(ret != 0) {
		log_error("Unable to download firmware component: %s \n", component_path);
		return -1;
	}

	return 0;
}


static int
ideviceenterramdisk_downloadimages() {
    log_info("Downloading firmware images...");
	  int i = 0;
    char *identifier = idevicedfu_info("product_type");

    while(device_loaders[i].identifier != NULL) {

        if(!strcmp(device_loaders[i].identifier, identifier)) {
    	       ipsw_loader = device_loaders[i];
             break;
        }

        i += 1;
    }

    if (ipsw_loader.ipsw_url) {

		const char *kernelcache_path = ipsw_loader.kernelcache_path,\
		*trustcache_path = ipsw_loader.trustcache_path,\
		*ramdisk_path = ipsw_loader.ramdisk_path,\
		*devicetree_path = ipsw_loader.devicetree_path,\
		*iBEC_path = ipsw_loader.ibec_path,\
		*iBSS_path = ipsw_loader.ibss_path;

		ipsw_url = (char*)ipsw_loader.ipsw_url;
    log_debug("Found IPSW loader : %s", ipsw_url);
		int ret;
		ret = download_firmware_component(kernelcache_path, kernelcache_save_path);

		if (ret == 0) {
  		  ret = download_firmware_component(trustcache_path, trustcache_save_path);

  			if (ret == 0) {
  				ret = download_firmware_component(ramdisk_path, ramdisk_save_path);

  				if (ret == 0) {
  					ret = download_firmware_component(devicetree_path, devicetree_save_path);

  					if (ret == 0) {
  						ret = download_firmware_component(iBEC_path, iBEC_save_path);

  						if (ret == 0) {
  							ret = download_firmware_component(iBSS_path, iBSS_save_path);
  						}
  					}
  				}
  			}
  		}

  		if (ret == 0) {
  			return 0;
  		}
    }

	return -1;
}

static int
ideviceenterramdisk_decryptimages() {
  log_info("Decrypting downloaded images...");
  char *dec_list[] = { kernelcache_save_path, trustcache_save_path, ramdisk_save_path, devicetree_save_path, NULL };
	char *cmd[CHAR_MAX];
  int ret = 0,
    i = 0;

    while(dec_list[i] != NULL) {

        if (ret != 0) {
            return ret;
        }

        sprintf(cmd, "img4 -i %s -o %s.dec;", dec_list[i], dec_list[i]);

        if (!execute_command(cmd)) {
            ret = -1;
        }

        i += 1;
    }

    if (ret == 0) {
        char *dec_save_path[sizeof iBEC_save_path + 5];
        sprintf(dec_save_path, "%s.dec", iBEC_save_path);
        ret = gastera1n_decrypt(iBEC_save_path, dec_save_path);

        if (ret == 0) {
            sprintf(dec_save_path, "%s.dec", iBSS_save_path);
            ret = gastera1n_decrypt(iBSS_save_path, dec_save_path);
        }
    }

	return ret;
}

static int
ideviceenterramdisk_patchimages()
{
    log_info("Patching images...");
    char *cmd[CHAR_MAX];

    if (access(tsschecker, F_OK) != 0) {
        sprintf(cmd, "gunzip %s.gz; xattr -d com.apple.quarantine %s >/dev/null 2>&1; chmod +x %s", tsschecker, tsschecker, tsschecker);
        execute_command(cmd);
    }

    char *ecid = idevicedfu_info("ecid");
    char *shsh2_path[sizeof rdsk_staging_path + 14];
    sprintf(shsh2_path, "%s/latest.shsh2", rdsk_staging_path);

    if (access(shsh2_path, F_OK) != 0) {
        sprintf(cmd, "./%s -e %s -d %s -B %s -b -l -s --save-path %s; mv %s/*shsh2 %s", tsschecker, ecid, idevicedfu_info("product_type"), idevicedfu_info("model"), rdsk_staging_path, rdsk_staging_path, shsh2_path);

        if (!execute_command(cmd) || !(access(shsh2_path, F_OK) == 0)) {
            return -1;
        }
    }

    size_t kdec_size = sizeof kernelcache_save_path + 5;
    char *kdec[kdec_size], *kpatched[kdec_size], *pdiff[sizeof rdsk_staging_path + 11];
    sprintf(kdec, "%s.dec", kernelcache_save_path);
    sprintf(kpatched, "%s.pwn", kernelcache_save_path);
    sprintf(pdiff, "%s/kc.bpatch", rdsk_staging_path);

    if (kernel64patcher_amfi(kdec, kpatched) != 0) {
        return -1;
    }

    if (kerneldiff(kdec, kpatched, pdiff) != 0) {
        return -1;
    }

    if (access("./iBoot64Patcher.gz", F_OK) == 0) {
        execute_command("gunzip iBoot64Patcher.gz; xattr -d com.apple.quarantine iBoot64Patcher >/dev/null 2>&1; chmod +x iBoot64Patcher;");
    }


    sprintf(cmd, "./iBoot64Patcher %s.dec %s.pwn -n -b \"rd=md0 -v\"", iBEC_save_path, iBEC_save_path);

    if(!execute_command(cmd)) {
        return -1;
    }

    sprintf(cmd, "./iBoot64Patcher %s.dec %s.pwn", iBSS_save_path, iBSS_save_path);

    if(!execute_command(cmd)) {
        return -1;
    }

    if (access("ssh64.tar.gz", F_OK) != 0) {
        // sprintf(cmd, "cat ssh64.tar.gz* > ssh64.tar.gz; rm ssh64.tar.gz_*;", rdsk_mount_path);
        sprintf(cmd, "cat ssh64.tar.gz* > ssh64.tar.gz;", rdsk_mount_path);
        execute_command(cmd);
    }

    if (access(ldid2, F_OK) != 0) {
        sprintf(cmd, "gunzip %s.gz; xattr -d com.apple.quarantine %s >/dev/null 2>&1; chmod +x %s", ldid2, ldid2, ldid2);
        execute_command(cmd);
    }

    if (access("restored_external.gz", F_OK) == 0) {
        execute_command("gunzip restored_external.gz");
    }

    int cmd_list_len = 8;
    char *commands[cmd_list_len][CHAR_MAX];

    sprintf(cmd, "cd %s; cp %s.dec ./rdsk.dmg;\
        hdiutil resize -size 180MB ./rdsk.dmg;\
        hdiutil attach ./rdsk.dmg -mountpoint %s;\
        sleep 5;", rdsk_staging_path, ramdisk_save_path, rdsk_mount_path);
    strcpy(commands[0], cmd);
    sprintf(cmd, "cd %s; echo \"WELCOME BACK!\" > ./etc/motd; mkdir -p ./private/var/root ./private/var/run ./sshd;", rdsk_mount_path);
    strcpy(commands[1], cmd);
    sprintf(cmd, "tar -C %s/sshd --preserve-permissions -xf ./ssh64.tar.gz;", rdsk_mount_path);
    strcpy(commands[2], cmd);
    sprintf(cmd, "cd %s/sshd; chmod 0755 ./bin/* && chmod 0755 ./usr/bin/* && chmod 0755 ./usr/sbin/* && chmod 0755 ./usr/local/bin/*;", rdsk_mount_path);
    strcpy(commands[3], cmd);
    sprintf(cmd, "cd %s/sshd; rsync --ignore-existing -auK . ../; sleep 2;", rdsk_mount_path);
    strcpy(commands[4], cmd);
    sprintf(cmd, "cd %s; rm -rf ./sshd ./usr/local/standalone/firmware/* ./usr/share/progressui ./usr/share/terminfo ./etc/apt/ ./etc/dpkg", rdsk_mount_path);
    strcpy(commands[5], cmd);

    sprintf(cmd, "cp ./restored_external ./restored_external_hax;\
        ./%s -e %s/usr/local/bin/restored_external > ./restored_external.plist;\
        ./%s -M -Srestored_external.plist ./restored_external_hax;\
        rm ./restored_external.plist;\
        mv ./restored_external_hax %s/usr/local/bin/restored_external;", ldid2, rdsk_mount_path, ldid2, rdsk_mount_path);
    strcpy(commands[6], cmd);
    sprintf(cmd, "hdiutil detach -force %s; sleep 5; hdiutil resize -sectors min %s/rdsk.dmg; sleep 3;", rdsk_mount_path, rdsk_staging_path);
    strcpy(commands[7], cmd);

    // int i;
    for (int i = 0; i < cmd_list_len; i++) {

        if (!execute_command(&commands[i])) {
            return -1;
        }
    }

    char* im4m_save_path[sizeof rdsk_staging_path + 6];
    sprintf(im4m_save_path, "%s/IM4M", rdsk_staging_path);
    im4m_from_shsh(shsh2_path, im4m_save_path);

    if (access(im4m_save_path, F_OK) != 0) {
        return -1;
    }

    sprintf(cmd, "cp bootim@750x1334.im4p %s/bootim.im4p; cd %s;\
        img4 -i ./bootim.im4p -o ./bootim.img4 -M ./IM4M", rdsk_staging_path, rdsk_staging_path);

    if (!execute_command(cmd)) {
        return -1;
    }

    sprintf(cmd, "cd %s;\
        img4 -i %s -o ./kernelcache.img4 -P ./kc.bpatch -M ./IM4M -T rkrn;\
        img4 -i %s -o ./trustcache.img4 -M ./IM4M -T rtsc;\
        img4 -i ./rdsk.dmg -o ./rdsk.img4 -M ./IM4M -A -T rdsk;\
        img4 -i %s -o ./dtree.img4 -M ./IM4M -T rdtr;\
        img4 -i %s.pwn -o ./ibec.img4 -A -M ./IM4M -T ibec;\
        img4 -i %s.pwn -o ./ibss.img4 -A -M ./IM4M -T ibss;", rdsk_staging_path, kernelcache_save_path, trustcache_save_path, devicetree_save_path, iBEC_save_path, iBSS_save_path);

    if (!execute_command(cmd)) {
        return -1;
    }

    return 0;
}

static int
ideviceenterramdisk_bootrd() {
    log_info("Booting device into SSH ramdisk mode...");
    int ret = 0;
    ret = gastera1n_reset();
    sleep(1);

    if (ret == 0) {

        idevicedfu_sendfile(iBSS_img4_path);
        sleep(1);
        idevicedfu_sendfile(iBEC_img4_path);
        sleep(1);
        idevicedfu_sendcommand("go");
        sleep(5);
        idevicedfu_sendfile(bootim_img4_path);
        idevicedfu_sendcommand("setpicture 0x1");
        idevicedfu_sendcommand("bgcolor 255 55 55");
        idevicedfu_sendfile(devicetree_img4_path);
        idevicedfu_sendcommand("devicetree");
        idevicedfu_sendfile(ramdisk_img4_path);
        idevicedfu_sendcommand("ramdisk");
        idevicedfu_sendfile(trustcache_img4_path);
        idevicedfu_sendcommand("firmware");
        idevicedfu_sendfile(kernelcache_img4_path);
        idevicedfu_sendcommand("bootx");
        log_info("Device should be booting now.");
    }

    return ret;
}

int
ideviceenterramdisk_load() {
	int ret = EXIT_SUCCESS;
	char *cmd[CHAR_MAX];

	if (ramdiskBootMode != 1) {
    sprintf(cmd, "bash -c 'if [ ! -d %s ];then mkdir -p %s; else rm %s/* >/dev/null 2>&1; fi'", rdsk_staging_path, rdsk_mount_path, rdsk_staging_path);
    execute_command(cmd);

    ret = ideviceenterramdisk_downloadimages();
    sleep(3);

    if (ret == 0) {

        if (access("img4.gz", F_OK) == 0) {
            if (access("/usr/local/bin/img4", F_OK) != 0) {
                execute_command("gunzip img4.gz; xattr -d com.apple.quarantine img4 >/dev/null 2>&1; chmod +x img4");
                execute_command("mv img4 /usr/local/bin/img4");
            }
        }

        ret = ideviceenterramdisk_decryptimages();
        sleep(3);
    }

    if (ret == 0) {
        ret = ideviceenterramdisk_patchimages();
    }
	} else {
    ret = 0;
  }

  if (ret == 0) {
      ret = ideviceenterramdisk_bootrd();
  }

	return ret;
}
