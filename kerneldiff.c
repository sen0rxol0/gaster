// kerneldiff original source: https://raw.githubusercontent.com/verygenericname/kerneldiff_C/refs/heads/main/kerneldiff.c
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <string.h>

#define MAX_DIFF 16384
#define DIFF_COMP_MAX_SIZE 20

static const char kerneldiff_amfi[] = "#AMFI\n\n";

static char*
kerneldiff_readfile(char *pathname, struct stat *st) {
  if (stat(pathname, &st) != 0) {
      perror("kerneldiff: Failed to get file status");
      return NULL;
  }

  FILE *fp = fopen(pathname, "rb");
  if (!fp) {
      perror("kerneldiff: Failed to open file");
      return NULL;
  }

  char *data = malloc(st.st_size);
  if (!data) {
      perror("kerneldiff: Failed to allocate memory");
      fclose(fp);
      return NULL;
  }

  fread(data, 1, st.st_size, fp);
  fclose(fp);
  return data;
}

int
kerneldiff(char *kc_original, char *kc_patched, char *kc_diff) {

  char diff[MAX_DIFF][3][DIFF_COMP_MAX_SIZE];
  int diff_idx = 0;

  struct stat st1, st2;
  char *o = kerneldiff_readfile(kc_original, &st1);
  char *p = kerneldiff_readfile(kc_patched, &st2);

  if (!o || !p) {
      free(o);
      free(p);
      return -1;
  }

  for (int i = 0; i < st1.st_size; i++) {
    if (o[i] != p[i]) {
        if (diff_idx >= MAX_DIFF) {
            fprintf(stderr, "kerneldiff: too many differences, only a maximum of %d differences are supported\n", MAX_DIFF);
            free(o);
            free(p);
            return -1;
        }
        snprintf(diff[diff_idx][0], DIFF_COMP_MAX_SIZE, "0x%x", i);
        snprintf(diff[diff_idx][1], DIFF_COMP_MAX_SIZE, "0x%x", (unsigned char)o[i]);
        snprintf(diff[diff_idx][2], DIFF_COMP_MAX_SIZE, "0x%x", (unsigned char)p[i]);
        diff_idx++;
    }
  }

  free(p);
  free(o);

  FILE *fp = fopen(kc_diff, "w+");
  fwrite(kerneldiff_amfi, 1, sizeof(kerneldiff_amfi) - 1, fp);

  for (int i = 0; i < diff_idx; i++) {
    printf("kerneldiff: %s %s %s\n", diff[i][0], diff[i][1], diff[i][2]);
    fprintf(fp, "%s %s %s\n", diff[i][0], diff[i][1], diff[i][2]);
  }

  fclose(fp);

  return 0;
}
