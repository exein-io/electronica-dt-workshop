# Copyright (c) 2026 Exein
# SPDX-License-Identifier: Apache-2.0
#
# This CMake file is picked by the Zephyr build system because it is defined
# as the module CMake entry point (see zephyr/module.yml).

zephyr_syscall_include_directories(include)
zephyr_include_directories(include)

if(CONFIG_RUNTIME_MONITOR)
  # ADR-010 D1 (amended): the credential store inside the archive calls the
  # PSA ITS API, so an ITS provider is a link prerequisite, and the archive
  # must have been built against the SAME provider (Zephyr secure_storage vs
  # TF-M resolve psa_its_* to different symbols). Checked here rather than as
  # a Kconfig `depends on`, which would loop through NETWORKING -> OpenThread
  # -> imply SECURE_STORAGE.
  if(NOT CONFIG_SECURE_STORAGE AND NOT CONFIG_BUILD_WITH_TFM)
    message(FATAL_ERROR "runtime-zephyr: CONFIG_RUNTIME_MONITOR needs an ITS "
      "provider: enable CONFIG_SECURE_STORAGE (with a store backend and the "
      "secure_storage_its_partition chosen node) or CONFIG_BUILD_WITH_TFM "
      "(+ CONFIG_TFM_PARTITION_INTERNAL_TRUSTED_STORAGE), matching the "
      "profile the blob was built with. See runtime_monitor.conf, item 3.")
  endif()

  # Blob mode: the single archive under zephyr/blobs/<core>/, placed there with
  # its sha256 in zephyr/module.yml by scripts/package_blob.sh.
  file(GLOB _blobs ${ZEPHYR_CURRENT_MODULE_DIR}/zephyr/blobs/*/libruntime_zephyr.a)
  if(EXISTS ${ZEPHYR_CURRENT_MODULE_DIR}/lib/src/agent.c)
    add_subdirectory(lib)
  elseif(_blobs)
    list(LENGTH _blobs _n_blobs)
    if(NOT _n_blobs EQUAL 1)
      message(FATAL_ERROR "runtime-zephyr: expected one blob under zephyr/blobs/, found: ${_blobs}")
    endif()
    set(_blob ${_blobs})
    # A wrong, stale or modified archive would link and misbehave at runtime.
    file(SHA256 ${_blob} _blob_sha)
    file(STRINGS ${ZEPHYR_CURRENT_MODULE_DIR}/zephyr/module.yml _yml_sha REGEX "sha256:")
    if(NOT _yml_sha MATCHES "${_blob_sha}")
      message(FATAL_ERROR "runtime-zephyr: ${_blob} does not match the sha256 in "
        "zephyr/module.yml (stale or modified blob)")
    endif()
    message(STATUS "runtime_zephyr: linking precompiled blob ${_blob}")
    # A Zephyr library like the others: linked --whole-archive (keeps the
    # iterable SYS_INIT/zbus sections) and ahead of the kernel, so its kernel
    # references resolve without KERNEL_WHOLE_ARCHIVE (LLEXT sets that).
    zephyr_library_import(runtime_zephyr ${_blob})
  else()
    message(FATAL_ERROR "runtime-zephyr: neither monitor sources nor a blob "
      "(zephyr/blobs/*/libruntime_zephyr.a) found")
  endif()

  if(CONFIG_RUNTIME_MONITOR_SYSCALL_HOOKS)
    zephyr_link_libraries(-Wl,--wrap=z_impl_k_thread_priority_set)
    zephyr_link_libraries(-Wl,--wrap=z_impl_k_yield)
    zephyr_link_libraries(-Wl,--wrap=z_impl_k_sleep)
    zephyr_link_libraries(-Wl,--wrap=z_impl_k_busy_wait)
    zephyr_link_libraries(-Wl,--wrap=arch_buffer_validate)
  endif()
endif()
