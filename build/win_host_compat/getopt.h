/* Copyright 2026 The Compass Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 *
 * getopt_long for building wayland-scanner with a Windows host toolchain,
 * which has no getopt.h (third_party-wayland-BUILD.gn.patch,
 * docs/building.md, "Linux from Windows"). Handles short options without
 * arguments, "--name" long options with no_argument or required_argument
 * ("--name=value" or "--name value"), and "--". Like GNU getopt, options may
 * follow operands: argv is permuted so they come first. */
#ifndef COMPASS_BUILD_WIN_HOST_COMPAT_GETOPT_H_
#define COMPASS_BUILD_WIN_HOST_COMPAT_GETOPT_H_

#include <stdio.h>
#include <string.h>

#define no_argument 0
#define required_argument 1
#define optional_argument 2

struct option {
  const char *name;
  int has_arg;
  int *flag;
  int val;
};

static char *optarg;
static int optind = 1, opterr = 1, optopt;
static int compass_optpos = 1;

static int getopt_long(int argc, char *const argv[], const char *optstring,
                       const struct option *longopts, int *longindex) {
  optarg = NULL;
  if (compass_optpos == 1) {
    /* Permute: bring the next option (and a separate argument of a
     * required_argument long option, rare here) to argv[optind]. */
    int j = optind;
    while (j < argc && (argv[j][0] != '-' || argv[j][1] == '\0'))
      j++;
    if (j >= argc)
      return -1;
    if (j > optind) {
      char **v = (char **)argv;
      char *opt = v[j];
      memmove(&v[optind + 1], &v[optind], (size_t)(j - optind) * sizeof(char *));
      v[optind] = opt;
    }
  }
  if (optind >= argc || argv[optind][0] != '-' || argv[optind][1] == '\0')
    return -1;
  if (strcmp(argv[optind], "--") == 0) {
    optind++;
    return -1;
  }
  if (argv[optind][1] == '-') {
    const char *arg = argv[optind] + 2;
    const char *eq = strchr(arg, '=');
    size_t len = eq ? (size_t)(eq - arg) : strlen(arg);
    optind++;
    for (int i = 0; longopts && longopts[i].name; i++) {
      if (strlen(longopts[i].name) != len ||
          strncmp(longopts[i].name, arg, len) != 0)
        continue;
      if (longindex)
        *longindex = i;
      if (longopts[i].has_arg == required_argument) {
        if (eq) {
          optarg = (char *)eq + 1;
        } else if (optind < argc) {
          optarg = argv[optind++];
        } else {
          if (opterr)
            fprintf(stderr, "%s: option '--%s' requires an argument\n",
                    argv[0], longopts[i].name);
          return '?';
        }
      } else if (longopts[i].has_arg == optional_argument && eq) {
        optarg = (char *)eq + 1;
      }
      if (longopts[i].flag) {
        *longopts[i].flag = longopts[i].val;
        return 0;
      }
      return longopts[i].val;
    }
    if (opterr)
      fprintf(stderr, "%s: unrecognized option '%s'\n", argv[0],
              argv[optind - 1]);
    return '?';
  }
  {
    char c = argv[optind][compass_optpos];
    const char *spec = strchr(optstring, c);
    if (argv[optind][++compass_optpos] == '\0') {
      optind++;
      compass_optpos = 1;
    }
    if (!spec || c == ':') {
      optopt = c;
      if (opterr)
        fprintf(stderr, "%s: invalid option -- '%c'\n", argv[0], c);
      return '?';
    }
    if (spec[1] == ':') {
      if (compass_optpos > 1) {
        optarg = argv[optind++] + compass_optpos;
        compass_optpos = 1;
      } else if (optind < argc) {
        optarg = argv[optind++];
      } else {
        optopt = c;
        return '?';
      }
    }
    return c;
  }
}

#endif  /* COMPASS_BUILD_WIN_HOST_COMPAT_GETOPT_H_ */
