/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "SandboxBrokerPolicyFactory.h"
#include "SandboxInfo.h"
#include "SandboxLogging.h"

#include "mozilla/ClearOnShutdown.h"
#include "mozilla/Preferences.h"
#include "mozilla/SandboxSettings.h"
#include "mozilla/dom/ContentChild.h"
#include "nsPrintfCString.h"
#include "nsString.h"
#include "nsThreadUtils.h"
#include "nsXULAppAPI.h"
#include "nsDirectoryServiceDefs.h"
#include "nsAppDirectoryServiceDefs.h"
#include "SpecialSystemDirectory.h"

#ifdef ANDROID
#include "cutils/properties.h"
#endif

#ifdef MOZ_WIDGET_GTK
#include <glib.h>
#endif

namespace mozilla {

#if defined(MOZ_CONTENT_SANDBOX)
namespace {
static const int rdonly = SandboxBroker::MAY_READ;
static const int wronly = SandboxBroker::MAY_WRITE;
static const int rdwr = rdonly | wronly;
static const int rdwrcr = rdwr | SandboxBroker::MAY_CREATE;
}
#endif

SandboxBrokerPolicyFactory::SandboxBrokerPolicyFactory()
{
  // Policy entries that are the same in every process go here, and
  // are cached over the lifetime of the factory.
#if defined(MOZ_CONTENT_SANDBOX)
  SandboxBroker::Policy* policy = new SandboxBroker::Policy;
  policy->AddDir(rdwrcr, "/dev/shm");
  // Write permssions
  //
  // Add write permissions on the temporary directory. This can come
  // from various environment variables (TMPDIR,TMP,TEMP,...) so
  // make sure to use the full logic.
  nsCOMPtr<nsIFile> tmpDir;
  nsresult rv = GetSpecialSystemDirectory(OS_TemporaryDirectory,
                                          getter_AddRefs(tmpDir));

  if (NS_SUCCEEDED(rv)) {
    nsAutoCString tmpPath;
    rv = tmpDir->GetNativePath(tmpPath);
    if (NS_SUCCEEDED(rv)) {
      policy->AddDir(rdwrcr, tmpPath.get());
    }
  }
  // If the above fails at any point, fall back to a very good guess.
  if (NS_FAILED(rv)) {
    policy->AddDir(rdwrcr, "/tmp");
  }

  // Bug 1308851: NVIDIA proprietary driver when using WebGL
  policy->AddFilePrefix(rdwr, "/dev", "nvidia");

  // Bug 1312678: radeonsi/Intel with DRI when using WebGL
  policy->AddDir(rdwr, "/dev/dri");

#ifdef MOZ_ALSA
  // Bug 1309098: ALSA support
  policy->AddDir(rdwr, "/dev/snd");
#endif

#ifdef MOZ_WIDGET_GTK
  if (const auto userDir = g_get_user_runtime_dir()) {
    // Bug 1321134: DConf's single bit of shared memory
    // The leaf filename is "user" by default, but is configurable.
    nsPrintfCString shmPath("%s/dconf/", userDir);
    policy->AddPrefix(rdwrcr, shmPath.get());
    policy->AddAncestors(shmPath.get());
#ifdef MOZ_PULSEAUDIO
    // PulseAudio, if it can't get server info from X11, will break
    // unless it can open this directory (or create it, but in our use
    // case we know it already exists).  See bug 1335329.
    nsPrintfCString pulsePath("%s/pulse", userDir);
    policy->AddPath(rdonly, pulsePath.get());
#endif // MOZ_PULSEAUDIO
  }
#endif // MOZ_WIDGET_GTK

  // Read permissions
  policy->AddPath(rdonly, "/dev/urandom");
  policy->AddPath(rdonly, "/proc/cpuinfo");
  policy->AddPath(rdonly, "/proc/meminfo");
  policy->AddDir(rdonly, "/sys/devices/cpu");
  policy->AddDir(rdonly, "/sys/devices/system/cpu");
  policy->AddDir(rdonly, "/lib");
  policy->AddDir(rdonly, "/lib64");
  policy->AddDir(rdonly, "/usr/lib");
  policy->AddDir(rdonly, "/usr/lib32");
  policy->AddDir(rdonly, "/usr/lib64");
  policy->AddDir(rdonly, "/etc");
#ifdef MOZ_PULSEAUDIO
  policy->AddPath(rdonly, "/var/lib/dbus/machine-id");
#endif
  policy->AddDir(rdonly, "/usr/share");
  policy->AddDir(rdonly, "/usr/local/share");
  policy->AddDir(rdonly, "/usr/tmp");
  policy->AddDir(rdonly, "/var/tmp");
  // Various places where fonts reside
  policy->AddDir(rdonly, "/usr/X11R6/lib/X11/fonts");
  policy->AddDir(rdonly, "/nix/store");
  policy->AddDir(rdonly, "/run/host/fonts");
  policy->AddDir(rdonly, "/run/host/user-fonts");

  // Bug 1384178: Mesa driver loader
  policy->AddPrefix(rdonly, "/sys/dev/char/226:");

  // Bug 1385715: NVIDIA PRIME support
  policy->AddPath(rdonly, "/proc/modules");

#ifdef MOZ_PULSEAUDIO
  // See bug 1384986 comment #1.
  if (const auto xauth = PR_GetEnv("XAUTHORITY")) {
    policy->AddPath(rdonly, xauth);
  }
#endif

  // Allow access to XDG_CONFIG_PATH and XDG_CONFIG_DIRS
  if (const auto xdgConfigPath = PR_GetEnv("XDG_CONFIG_PATH")) {
    policy->AddDir(rdonly, xdgConfigPath);
  }

  nsAutoCString xdgConfigDirs(PR_GetEnv("XDG_CONFIG_DIRS"));
  for (const auto& path : xdgConfigDirs.Split(':')) {
    policy->AddDir(rdonly, PromiseFlatCString(path).get());
  }

  // Extra configuration dirs in the homedir that we want to allow read
  // access to.
  mozilla::Array<const char*, 3> extraConfDirs = {
    ".config",   // Fallback if XDG_CONFIG_PATH isn't set
    ".themes",
    ".fonts",
  };

  nsCOMPtr<nsIFile> homeDir;
  rv = GetSpecialSystemDirectory(Unix_HomeDirectory, getter_AddRefs(homeDir));
  if (NS_SUCCEEDED(rv)) {
    nsCOMPtr<nsIFile> confDir;

    for (const auto& dir : extraConfDirs) {
      rv = homeDir->Clone(getter_AddRefs(confDir));
      if (NS_SUCCEEDED(rv)) {
        rv = confDir->AppendNative(nsDependentCString(dir));
        if (NS_SUCCEEDED(rv)) {
          nsAutoCString tmpPath;
          rv = confDir->GetNativePath(tmpPath);
          if (NS_SUCCEEDED(rv)) {
            policy->AddDir(rdonly, tmpPath.get());
          }
        }
      }
    }

    // ~/.local/share (for themes)
    rv = homeDir->Clone(getter_AddRefs(confDir));
    if (NS_SUCCEEDED(rv)) {
      rv = confDir->AppendNative(NS_LITERAL_CSTRING(".local"));
      if (NS_SUCCEEDED(rv)) {
        rv = confDir->AppendNative(NS_LITERAL_CSTRING("share"));
      }
      if (NS_SUCCEEDED(rv)) {
        nsAutoCString tmpPath;
        rv = confDir->GetNativePath(tmpPath);
        if (NS_SUCCEEDED(rv)) {
          policy->AddDir(rdonly, tmpPath.get());
        }
      }
    }

    // ~/.fonts.conf (Fontconfig)
    rv = homeDir->Clone(getter_AddRefs(confDir));
    if (NS_SUCCEEDED(rv)) {
      rv = confDir->AppendNative(NS_LITERAL_CSTRING(".fonts.conf"));
      if (NS_SUCCEEDED(rv)) {
        nsAutoCString tmpPath;
        rv = confDir->GetNativePath(tmpPath);
        if (NS_SUCCEEDED(rv)) {
          policy->AddPath(rdonly, tmpPath.get());
        }
      }
    }

    // .pangorc
    rv = homeDir->Clone(getter_AddRefs(confDir));
    if (NS_SUCCEEDED(rv)) {
      rv = confDir->AppendNative(NS_LITERAL_CSTRING(".pangorc"));
      if (NS_SUCCEEDED(rv)) {
        nsAutoCString tmpPath;
        rv = confDir->GetNativePath(tmpPath);
        if (NS_SUCCEEDED(rv)) {
          policy->AddPath(rdonly, tmpPath.get());
        }
      }
    }
  }

  // Firefox binary dir.
  // Note that unlike the previous cases, we use NS_GetSpecialDirectory
  // instead of GetSpecialSystemDirectory. The former requires a working XPCOM
  // system, which may not be the case for some tests. For quering for the
  // location of XPCOM things, we can use it anyway.
  nsCOMPtr<nsIFile> ffDir;
  rv = NS_GetSpecialDirectory(NS_GRE_DIR, getter_AddRefs(ffDir));
  if (NS_SUCCEEDED(rv)) {
    nsAutoCString tmpPath;
    rv = ffDir->GetNativePath(tmpPath);
    if (NS_SUCCEEDED(rv)) {
      policy->AddDir(rdonly, tmpPath.get());
    }
  }

  if (mozilla::IsDevelopmentBuild()) {
    // If this is a developer build the resources are symlinks to outside the binary dir.
    // Therefore in non-release builds we allow reads from the whole repository.
    // MOZ_DEVELOPER_REPO_DIR is set by mach run.
    const char *developer_repo_dir = PR_GetEnv("MOZ_DEVELOPER_REPO_DIR");
    if (developer_repo_dir) {
      policy->AddDir(rdonly, developer_repo_dir);
    }
  }

  mCommonContentPolicy.reset(policy);
#endif
}

#ifdef MOZ_CONTENT_SANDBOX
UniquePtr<SandboxBroker::Policy>
SandboxBrokerPolicyFactory::GetContentPolicy(int aPid, bool aFileProcess)
{
  // Policy entries that vary per-process (currently the only reason
  // that can happen is because they contain the pid) are added here,
  // as well as entries that depend on preferences or paths not available
  // in early startup.

  MOZ_ASSERT(NS_IsMainThread());
  // File broker usage is controlled through a pref.
  if (GetEffectiveContentSandboxLevel() <= 1) {
    return nullptr;
  }

  MOZ_ASSERT(mCommonContentPolicy);
  UniquePtr<SandboxBroker::Policy>
    policy(new SandboxBroker::Policy(*mCommonContentPolicy));

  // Read any extra paths that will get write permissions,
  // configured by the user or distro
  AddDynamicPathList(policy.get(),
                     "security.sandbox.content.write_path_whitelist",
                     rdwr);

  // Whitelisted for reading by the user/distro
  AddDynamicPathList(policy.get(),
                    "security.sandbox.content.read_path_whitelist",
                    rdonly);

  // No read blocking at level 2 and below.
  // file:// processes also get global read permissions
  // This requires accessing user preferences so we can only do it now.
  // Our constructor is initialized before user preferences are read in.
  if (GetEffectiveContentSandboxLevel() <= 2 || aFileProcess) {
    policy->AddDir(rdonly, "/");
    // Any other read-only rules will be removed as redundant by
    // Policy::FixRecursivePermissions, so there's no need to
    // early-return here.
  }

  // Bug 1198550: the profiler's replacement for dl_iterate_phdr
  policy->AddPath(rdonly, nsPrintfCString("/proc/%d/maps", aPid).get());

  // Bug 1198552: memory reporting.
  policy->AddPath(rdonly, nsPrintfCString("/proc/%d/statm", aPid).get());
  policy->AddPath(rdonly, nsPrintfCString("/proc/%d/smaps", aPid).get());

  // Bug 1384804, notably comment 15
  // Used by libnuma, included by x265/ffmpeg, who falls back
  // to get_mempolicy if this fails
  policy->AddPath(rdonly, nsPrintfCString("/proc/%d/status", aPid).get());

  // userContent.css and the extensions dir sit in the profile, which is
  // normally blocked and we can't get the profile dir earlier in startup,
  // so this must happen here.
  nsCOMPtr<nsIFile> profileDir;
  nsresult rv = NS_GetSpecialDirectory(NS_APP_USER_PROFILE_50_DIR,
                                       getter_AddRefs(profileDir));
  if (NS_SUCCEEDED(rv)) {
      nsCOMPtr<nsIFile> workDir;
      rv = profileDir->Clone(getter_AddRefs(workDir));
      if (NS_SUCCEEDED(rv)) {
        rv = workDir->AppendNative(NS_LITERAL_CSTRING("chrome"));
        if (NS_SUCCEEDED(rv)) {
          rv = workDir->AppendNative(NS_LITERAL_CSTRING("userContent.css"));
          if (NS_SUCCEEDED(rv)) {
            nsAutoCString tmpPath;
            rv = workDir->GetNativePath(tmpPath);
            if (NS_SUCCEEDED(rv)) {
              policy->AddPath(rdonly, tmpPath.get());
            }
          }
        }
      }
      rv = profileDir->Clone(getter_AddRefs(workDir));
      if (NS_SUCCEEDED(rv)) {
        rv = workDir->AppendNative(NS_LITERAL_CSTRING("extensions"));
        if (NS_SUCCEEDED(rv)) {
          nsAutoCString tmpPath;
          rv = workDir->GetNativePath(tmpPath);
          if (NS_SUCCEEDED(rv)) {
            policy->AddDir(rdonly, tmpPath.get());
          }
        }
      }
  }

  // Return the common policy.
  policy->FixRecursivePermissions();
  return policy;
}

void
SandboxBrokerPolicyFactory::AddDynamicPathList(SandboxBroker::Policy *policy,
                                               const char* aPathListPref,
                                               int perms)
{
  nsAutoCString pathList;
  nsresult rv = Preferences::GetCString(aPathListPref, pathList);
  if (NS_SUCCEEDED(rv)) {
    for (const nsACString& path : pathList.Split(',')) {
      nsCString trimPath(path);
      trimPath.Trim(" ", true, true);
      policy->AddDynamic(perms, trimPath.get());
    }
  }
}

#endif // MOZ_CONTENT_SANDBOX
} // namespace mozilla
