// clang-format off
// =============================================================================
// XNU kperf/kpc demo
// Available for 64-bit Intel/Apple Silicon, macOS/iOS, with root privileges
//
//
// Demo 1 (profile a function in current thread):
// 1. Open directory '/usr/share/kpep/', find your CPU PMC database.
//    For M1 (Pro/Max), the database file is '/usr/share/kpep/a14.plist'.
// 2. Select a few events that you are interested in,
//    add their names to the `profile_events` array below.
// 3. Put your code in `profile_func` function below.
// 4. Compile and run with root (sudo).
//
//
// Demo 2 (profile a select process):
// Replace step 3 with: set `target_pid` and `total_profile_time`.
// Use main2() as the entry function.
//
//
// References:
//
// XNU source (since xnu 2422.1.72):
// https://github.com/apple/darwin-xnu/blob/main/osfmk/kern/kpc.h
// https://github.com/apple/darwin-xnu/blob/main/bsd/kern/kern_kpc.c
//
// Lightweight PET (Profile Every Thread, since xnu 3789.1.32):
// https://github.com/apple/darwin-xnu/blob/main/osfmk/kperf/pet.c
// https://github.com/apple/darwin-xnu/blob/main/osfmk/kperf/kperf_kpc.c
//
// System Private frameworks (since macOS 10.11, iOS 8.0):
// /System/Library/PrivateFrameworks/kperf.framework
// /System/Library/PrivateFrameworks/kperfdata.framework
//
// Xcode framework (since Xcode 7.0):
// /Applications/Xcode.app/Contents/SharedFrameworks/DVTInstrumentsFoundation.framework
//
// CPU database (plist files)
// macOS (since macOS 10.11):
//     /usr/share/kpep/<name>.plist
// iOS (copied from Xcode, since iOS 10.0, Xcode 8.0):
//     /Applications/Xcode.app/Contents/Developer/Platforms/iPhoneOS.platform
//     /DeviceSupport/<version>/DeveloperDiskImage.dmg/usr/share/kpep/<name>.plist
//
//
// Created by YaoYuan <ibireme@gmail.com> on 2021.
// Released into the public domain (unlicense.org).
// =============================================================================

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <dlfcn.h>          // for dlopen() and dlsym()
#include <mach/mach_time.h> // for mach_absolute_time()
#include <sys/kdebug.h>     // for kdebug trace decode
#include <sys/sysctl.h>     // for sysctl()
#include <unistd.h>         // for usleep()

typedef float f32;
typedef double f64;
typedef int8_t i8;
typedef uint8_t u8;
typedef int16_t i16;
typedef uint16_t u16;
typedef int32_t i32;
typedef uint32_t u32;
typedef int64_t i64;
typedef uint64_t u64;
typedef size_t usize;

// -----------------------------------------------------------------------------
// <kperf.framework> header (reverse engineered)
// This framework wraps some sysctl calls to communicate with the kpc in kernel.
// Most functions requires root privileges, or process is "blessed".
// -----------------------------------------------------------------------------

// Cross-platform class constants.
#define KPC_CLASS_FIXED (0)
#define KPC_CLASS_CONFIGURABLE (1)
#define KPC_CLASS_POWER (2)
#define KPC_CLASS_RAWPMU (3)

// Cross-platform class mask constants.
#define KPC_CLASS_FIXED_MASK (1u << KPC_CLASS_FIXED)               // 1
#define KPC_CLASS_CONFIGURABLE_MASK (1u << KPC_CLASS_CONFIGURABLE) // 2
#define KPC_CLASS_POWER_MASK (1u << KPC_CLASS_POWER)               // 4
#define KPC_CLASS_RAWPMU_MASK (1u << KPC_CLASS_RAWPMU)             // 8

// PMU version constants.
#define KPC_PMU_ERROR (0)     // Error
#define KPC_PMU_INTEL_V3 (1)  // Intel
#define KPC_PMU_ARM_APPLE (2) // ARM64
#define KPC_PMU_INTEL_V2 (3)  // Old Intel
#define KPC_PMU_ARM_V2 (4)    // Old ARM

// The maximum number of counters we could read from every class in one go.
// ARMV7: FIXED: 1, CONFIGURABLE: 4
// ARM32: FIXED: 2, CONFIGURABLE: 6
// ARM64: FIXED: 2, CONFIGURABLE: CORE_NCTRS - FIXED (6 or 8)
// x86: 32
#define KPC_MAX_COUNTERS 32

// Bits for defining what to do on an action.
// Defined in https://github.com/apple/darwin-xnu/blob/main/osfmk/kperf/action.h
#define KPERF_SAMPLER_TH_INFO (1U << 0)
#define KPERF_SAMPLER_TH_SNAPSHOT (1U << 1)
#define KPERF_SAMPLER_KSTACK (1U << 2)
#define KPERF_SAMPLER_USTACK (1U << 3)
#define KPERF_SAMPLER_PMC_THREAD (1U << 4)
#define KPERF_SAMPLER_PMC_CPU (1U << 5)
#define KPERF_SAMPLER_PMC_CONFIG (1U << 6)
#define KPERF_SAMPLER_MEMINFO (1U << 7)
#define KPERF_SAMPLER_TH_SCHEDULING (1U << 8)
#define KPERF_SAMPLER_TH_DISPATCH (1U << 9)
#define KPERF_SAMPLER_TK_SNAPSHOT (1U << 10)
#define KPERF_SAMPLER_SYS_MEM (1U << 11)
#define KPERF_SAMPLER_TH_INSCYC (1U << 12)
#define KPERF_SAMPLER_TK_INFO (1U << 13)

// Maximum number of kperf action ids.
#define KPERF_ACTION_MAX (32)

// Maximum number of kperf timer ids.
#define KPERF_TIMER_MAX (8)

// x86/arm config registers are 64-bit
typedef u64 kpc_config_t;

/// Print current CPU identification string to the buffer (same as snprintf),
/// such as "cpu_7_8_10b282dc_46". This string can be used to locate the PMC
/// database in /usr/share/kpep.
/// @return string's length, or negative value if error occurs.
/// @note This method does not requires root privileges.
/// @details sysctl get(hw.cputype), get(hw.cpusubtype),
///                 get(hw.cpufamily), get(machdep.cpu.model)
static int (*kpc_cpu_string)(char *buf, usize buf_size);

/// Get the version of KPC that's being run.
/// @return See `PMU version constants` above.
/// @details sysctl get(kpc.pmu_version)
static u32 (*kpc_pmu_version)(void);

/// Get running PMC classes.
/// @return See `class mask constants` above,
///         0 if error occurs or no class is set.
/// @details sysctl get(kpc.counting)
static u32 (*kpc_get_counting)(void);

/// Set PMC classes to enable counting.
/// @param classes See `class mask constants` above, set 0 to shutdown counting.
/// @return 0 for success.
/// @details sysctl set(kpc.counting)
static int (*kpc_set_counting)(u32 classes);

/// Get running PMC classes for current thread.
/// @return See `class mask constants` above,
///         0 if error occurs or no class is set.
/// @details sysctl get(kpc.thread_counting)
static u32 (*kpc_get_thread_counting)(void);

/// Set PMC classes to enable counting for current thread.
/// @param classes See `class mask constants` above, set 0 to shutdown counting.
/// @return 0 for success.
/// @details sysctl set(kpc.thread_counting)
static int (*kpc_set_thread_counting)(u32 classes);

/// Get how many config registers there are for a given mask.
/// For example: Intel may returns 1 for `KPC_CLASS_FIXED_MASK`,
///                        returns 4 for `KPC_CLASS_CONFIGURABLE_MASK`.
/// @param classes See `class mask constants` above.
/// @return 0 if error occurs or no class is set.
/// @note This method does not requires root privileges.
/// @details sysctl get(kpc.config_count)
static u32 (*kpc_get_config_count)(u32 classes);

/// Get config registers.
/// @param classes see `class mask constants` above.
/// @param config Config buffer to receive values, should not smaller than
///               kpc_get_config_count(classes) * sizeof(kpc_config_t).
/// @return 0 for success.
/// @details sysctl get(kpc.config_count), get(kpc.config)
static int (*kpc_get_config)(u32 classes, kpc_config_t *config);

/// Set config registers.
/// @param classes see `class mask constants` above.
/// @param config Config buffer, should not smaller than
///               kpc_get_config_count(classes) * sizeof(kpc_config_t).
/// @return 0 for success.
/// @details sysctl get(kpc.config_count), set(kpc.config)
static int (*kpc_set_config)(u32 classes, kpc_config_t *config);

/// Get how many counters there are for a given mask.
/// For example: Intel may returns 3 for `KPC_CLASS_FIXED_MASK`,
///                        returns 4 for `KPC_CLASS_CONFIGURABLE_MASK`.
/// @param classes See `class mask constants` above.
/// @note This method does not requires root privileges.
/// @details sysctl get(kpc.counter_count)
static u32 (*kpc_get_counter_count)(u32 classes);

/// Get counter accumulations.
/// If `all_cpus` is true, the buffer count should not smaller than
/// (cpu_count * counter_count). Otherwize, the buffer count should not smaller
/// than (counter_count).
/// @see kpc_get_counter_count(), kpc_cpu_count().
/// @param all_cpus true for all CPUs, false for current cpu.
/// @param classes See `class mask constants` above.
/// @param curcpu A pointer to receive current cpu id, can be NULL.
/// @param buf Buffer to receive counter's value.
/// @return 0 for success.
/// @details sysctl get(hw.ncpu), get(kpc.counter_count), get(kpc.counters)
static int (*kpc_get_cpu_counters)(bool all_cpus, u32 classes, int *curcpu,
                                   u64 *buf);

/// Get counter accumulations for current thread.
/// @param tid Thread id, should be 0.
/// @param buf_count The number of buf's elements (not bytes),
///                  should not smaller than kpc_get_counter_count().
/// @param buf Buffer to receive counter's value.
/// @return 0 for success.
/// @details sysctl get(kpc.thread_counters)
static int (*kpc_get_thread_counters)(u32 tid, u32 buf_count, u64 *buf);

/// Acquire/release the counters used by the Power Manager.
/// @param val 1:acquire, 0:release
/// @return 0 for success.
/// @details sysctl set(kpc.force_all_ctrs)
static int (*kpc_force_all_ctrs_set)(int val);

/// Get the state of all_ctrs.
/// @return 0 for success.
/// @details sysctl get(kpc.force_all_ctrs)
static int (*kpc_force_all_ctrs_get)(int *val_out);

/// Set number of actions, should be `KPERF_ACTION_MAX`.
/// @details sysctl set(kperf.action.count)
static int (*kperf_action_count_set)(u32 count);

/// Get number of actions.
/// @details sysctl get(kperf.action.count)
static int (*kperf_action_count_get)(u32 *count);

/// Set what to sample when a trigger fires an action, e.g.
/// `KPERF_SAMPLER_PMC_CPU`.
/// @details sysctl set(kperf.action.samplers)
static int (*kperf_action_samplers_set)(u32 actionid, u32 sample);

/// Get what to sample when a trigger fires an action.
/// @details sysctl get(kperf.action.samplers)
static int (*kperf_action_samplers_get)(u32 actionid, u32 *sample);

/// Apply a task filter to the action, -1 to disable filter.
/// @details sysctl set(kperf.action.filter_by_task)
static int (*kperf_action_filter_set_by_task)(u32 actionid, i32 port);

/// Apply a pid filter to the action, -1 to disable filter.
/// @details sysctl set(kperf.action.filter_by_pid)
static int (*kperf_action_filter_set_by_pid)(u32 actionid, i32 pid);

/// Set number of time triggers, should be `KPERF_TIMER_MAX`.
/// @details sysctl set(kperf.timer.count)
static int (*kperf_timer_count_set)(u32 count);

/// Get number of time triggers.
/// @details sysctl get(kperf.timer.count)
static int (*kperf_timer_count_get)(u32 *count);

/// Set timer number and period.
/// @details sysctl set(kperf.timer.period)
static int (*kperf_timer_period_set)(u32 actionid, u64 tick);

/// Get timer number and period.
/// @details sysctl get(kperf.timer.period)
static int (*kperf_timer_period_get)(u32 actionid, u64 *tick);

/// Set timer number and actionid.
/// @details sysctl set(kperf.timer.action)
static int (*kperf_timer_action_set)(u32 actionid, u32 timerid);

/// Get timer number and actionid.
/// @details sysctl get(kperf.timer.action)
static int (*kperf_timer_action_get)(u32 actionid, u32 *timerid);

/// Set which timer ID does PET (Profile Every Thread).
/// @details sysctl set(kperf.timer.pet_timer)
static int (*kperf_timer_pet_set)(u32 timerid);

/// Get which timer ID does PET (Profile Every Thread).
/// @details sysctl get(kperf.timer.pet_timer)
static int (*kperf_timer_pet_get)(u32 *timerid);

/// Enable or disable sampling.
/// @details sysctl set(kperf.sampling)
static int (*kperf_sample_set)(u32 enabled);

/// Get is currently sampling.
/// @details sysctl get(kperf.sampling)
static int (*kperf_sample_get)(u32 *enabled);

/// Reset kperf: stop sampling, kdebug, timers and actions.
/// @return 0 for success.
static int (*kperf_reset)(void);

/// Nanoseconds to CPU ticks.
static u64 (*kperf_ns_to_ticks)(u64 ns);

/// CPU ticks to nanoseconds.
static u64 (*kperf_ticks_to_ns)(u64 ticks);

/// CPU ticks frequency (mach_absolute_time).
static u64 (*kperf_tick_frequency)(void);

/// Get lightweight PET mode (not in kperf.framework).
static int kperf_lightweight_pet_get(u32 *enabled) {
  if (!enabled)
    return -1;
  usize size = 4;
  return sysctlbyname("kperf.lightweight_pet", enabled, &size, NULL, 0);
}

/// Set lightweight PET mode (not in kperf.framework).
static int kperf_lightweight_pet_set(u32 enabled) {
  return sysctlbyname("kperf.lightweight_pet", NULL, NULL, &enabled, 4);
}

// -----------------------------------------------------------------------------
// <kperfdata.framework> header (reverse engineered)
// This framework provides some functions to access the local CPU database.
// These functions do not require root privileges.
// -----------------------------------------------------------------------------

// KPEP CPU archtecture constants.
#define KPEP_ARCH_I386 0
#define KPEP_ARCH_X86_64 1
#define KPEP_ARCH_ARM 2
#define KPEP_ARCH_ARM64 3

/// KPEP event (size: 48/28 bytes on 64/32 bit OS)
typedef struct kpep_event {
  const char *name; ///< Unique name of a event, such as "INST_RETIRED.ANY".
  const char *description; ///< Description for this event.
  const char *errata;      ///< Errata, currently NULL.
  const char *alias;       ///< Alias name, such as "Instructions", "Cycles".
  const char *fallback;    ///< Fallback event name for fixed counter.
  u32 mask;
  u8 number;
  u8 umask;
  u8 reserved;
  u8 is_fixed;
} kpep_event;

/// KPEP database (size: 144/80 bytes on 64/32 bit OS)
typedef struct kpep_db {
  const char *name;           ///< Database name, such as "haswell".
  const char *cpu_id;         ///< Plist name, such as "cpu_7_8_10b282dc".
  const char *marketing_name; ///< Marketing name, such as "Intel Haswell".
  void *plist_data;           ///< Plist data (CFDataRef), currently NULL.
  void *event_map; ///< All events (CFDict<CFSTR(event_name), kpep_event *>).
  kpep_event
      *event_arr; ///< Event struct buffer (sizeof(kpep_event) * events_count).
  kpep_event **fixed_event_arr; ///< Fixed counter events (sizeof(kpep_event *)
                                ///< * fixed_counter_count)
  void *alias_map; ///< All aliases (CFDict<CFSTR(event_name), kpep_event *>).
  usize reserved_1;
  usize reserved_2;
  usize reserved_3;
  usize event_count; ///< All events count.
  usize alias_count;
  usize fixed_counter_count;
  usize config_counter_count;
  usize power_counter_count;
  u32 archtecture; ///< see `KPEP CPU archtecture constants` above.
  u32 fixed_counter_bits;
  u32 config_counter_bits;
  u32 power_counter_bits;
} kpep_db;

/// KPEP config (size: 80/44 bytes on 64/32 bit OS)
typedef struct kpep_config {
  kpep_db *db;
  kpep_event **ev_arr; ///< (sizeof(kpep_event *) * counter_count), init NULL
  usize *ev_map;       ///< (sizeof(usize *) * counter_count), init 0
  usize *ev_idx;       ///< (sizeof(usize *) * counter_count), init -1
  u32 *flags;          ///< (sizeof(u32 *) * counter_count), init 0
  u64 *kpc_periods;    ///< (sizeof(u64 *) * counter_count), init 0
  usize event_count;   /// kpep_config_events_count()
  usize counter_count;
  u32 classes; ///< See `class mask constants` above.
  u32 config_counter;
  u32 power_counter;
  u32 reserved;
} kpep_config;

/// Error code for kpep_config_xxx() and kpep_db_xxx() functions.
typedef enum {
  KPEP_CONFIG_ERROR_NONE = 0,
  KPEP_CONFIG_ERROR_INVALID_ARGUMENT = 1,
  KPEP_CONFIG_ERROR_OUT_OF_MEMORY = 2,
  KPEP_CONFIG_ERROR_IO = 3,
  KPEP_CONFIG_ERROR_BUFFER_TOO_SMALL = 4,
  KPEP_CONFIG_ERROR_CUR_SYSTEM_UNKNOWN = 5,
  KPEP_CONFIG_ERROR_DB_PATH_INVALID = 6,
  KPEP_CONFIG_ERROR_DB_NOT_FOUND = 7,
  KPEP_CONFIG_ERROR_DB_ARCH_UNSUPPORTED = 8,
  KPEP_CONFIG_ERROR_DB_VERSION_UNSUPPORTED = 9,
  KPEP_CONFIG_ERROR_DB_CORRUPT = 10,
  KPEP_CONFIG_ERROR_EVENT_NOT_FOUND = 11,
  KPEP_CONFIG_ERROR_CONFLICTING_EVENTS = 12,
  KPEP_CONFIG_ERROR_COUNTERS_NOT_FORCED = 13,
  KPEP_CONFIG_ERROR_EVENT_UNAVAILABLE = 14,
  KPEP_CONFIG_ERROR_ERRNO = 15,
  KPEP_CONFIG_ERROR_MAX
} kpep_config_error_code;

/// Error description for kpep_config_error_code.
static const char *kpep_config_error_names[KPEP_CONFIG_ERROR_MAX] = {
    "none",
    "invalid argument",
    "out of memory",
    "I/O",
    "buffer too small",
    "current system unknown",
    "database path invalid",
    "database not found",
    "database architecture unsupported",
    "database version unsupported",
    "database corrupt",
    "event not found",
    "conflicting events",
    "all counters must be forced",
    "event unavailable",
    "check errno"};

/// Error description.
static const char *kpep_config_error_desc(int code) {
  if (0 <= code && code < KPEP_CONFIG_ERROR_MAX) {
    return kpep_config_error_names[code];
  }
  return "unknown error";
}

/// Create a config.
/// @param db A kpep db, see kpep_db_create()
/// @param cfg_ptr A pointer to receive the new config.
/// @return kpep_config_error_code, 0 for success.
static int (*kpep_config_create)(kpep_db *db, kpep_config **cfg_ptr);

/// Free the config.
static void (*kpep_config_free)(kpep_config *cfg);

/// Add an event to config.
/// @param cfg The config.
/// @param ev_ptr A event pointer.
/// @param flag 0: all, 1: user space only
/// @param err Error bitmap pointer, can be NULL.
///            If return value is `CONFLICTING_EVENTS`, this bitmap contains
///            the conflicted event indices, e.g. "1 << 2" means index 2.
/// @return kpep_config_error_code, 0 for success.
static int (*kpep_config_add_event)(kpep_config *cfg, kpep_event **ev_ptr,
                                    u32 flag, u32 *err);

/// Remove event at index.
/// @return kpep_config_error_code, 0 for success.
static int (*kpep_config_remove_event)(kpep_config *cfg, usize idx);

/// Force all counters.
/// @return kpep_config_error_code, 0 for success.
static int (*kpep_config_force_counters)(kpep_config *cfg);

/// Get events count.
/// @return kpep_config_error_code, 0 for success.
static int (*kpep_config_events_count)(kpep_config *cfg, usize *count_ptr);

/// Get all event pointers.
/// @param buf A buffer to receive event pointers.
/// @param buf_size The buffer's size in bytes, should not smaller than
///                 kpep_config_events_count() * sizeof(void *).
/// @return kpep_config_error_code, 0 for success.
static int (*kpep_config_events)(kpep_config *cfg, kpep_event **buf,
                                 usize buf_size);

/// Get kpc register configs.
/// @param buf A buffer to receive kpc register configs.
/// @param buf_size The buffer's size in bytes, should not smaller than
///                 kpep_config_kpc_count() * sizeof(kpc_config_t).
/// @return kpep_config_error_code, 0 for success.
static int (*kpep_config_kpc)(kpep_config *cfg, kpc_config_t *buf,
                              usize buf_size);

/// Get kpc register config count.
/// @return kpep_config_error_code, 0 for success.
static int (*kpep_config_kpc_count)(kpep_config *cfg, usize *count_ptr);

/// Get kpc classes.
/// @param classes See `class mask constants` above.
/// @return kpep_config_error_code, 0 for success.
static int (*kpep_config_kpc_classes)(kpep_config *cfg, u32 *classes_ptr);

/// Get the index mapping from event to counter.
/// @param buf A buffer to receive indexes.
/// @param buf_size The buffer's size in bytes, should not smaller than
///                 kpep_config_events_count() * sizeof(kpc_config_t).
/// @return kpep_config_error_code, 0 for success.
static int (*kpep_config_kpc_map)(kpep_config *cfg, usize *buf, usize buf_size);

/// Open a kpep database file in "/usr/share/kpep/" or "/usr/local/share/kpep/".
/// @param name File name, for example "haswell", "cpu_100000c_1_92fb37c8".
///             Pass NULL for current CPU.
/// @return kpep_config_error_code, 0 for success.
static int (*kpep_db_create)(const char *name, kpep_db **db_ptr);

/// Free the kpep database.
static void (*kpep_db_free)(kpep_db *db);

/// Get the database's name.
/// @return kpep_config_error_code, 0 for success.
static int (*kpep_db_name)(kpep_db *db, const char **name);

/// Get the event alias count.
/// @return kpep_config_error_code, 0 for success.
static int (*kpep_db_aliases_count)(kpep_db *db, usize *count);

/// Get all alias.
/// @param buf A buffer to receive all alias strings.
/// @param buf_size The buffer's size in bytes,
///        should not smaller than kpep_db_aliases_count() * sizeof(void *).
/// @return kpep_config_error_code, 0 for success.
static int (*kpep_db_aliases)(kpep_db *db, const char **buf, usize buf_size);

/// Get counters count for given classes.
/// @param classes 1: Fixed, 2: Configurable.
/// @return kpep_config_error_code, 0 for success.
static int (*kpep_db_counters_count)(kpep_db *db, u8 classes, usize *count);

/// Get all event count.
/// @return kpep_config_error_code, 0 for success.
static int (*kpep_db_events_count)(kpep_db *db, usize *count);

/// Get all events.
/// @param buf A buffer to receive all event pointers.
/// @param buf_size The buffer's size in bytes,
///        should not smaller than kpep_db_events_count() * sizeof(void *).
/// @return kpep_config_error_code, 0 for success.
static int (*kpep_db_events)(kpep_db *db, kpep_event **buf, usize buf_size);

/// Get one event by name.
/// @return kpep_config_error_code, 0 for success.
static int (*kpep_db_event)(kpep_db *db, const char *name, kpep_event **ev_ptr);

/// Get event's name.
/// @return kpep_config_error_code, 0 for success.
static int (*kpep_event_name)(kpep_event *ev, const char **name_ptr);

/// Get event's alias.
/// @return kpep_config_error_code, 0 for success.
static int (*kpep_event_alias)(kpep_event *ev, const char **alias_ptr);

/// Get event's description.
/// @return kpep_config_error_code, 0 for success.
static int (*kpep_event_description)(kpep_event *ev, const char **str_ptr);

// -----------------------------------------------------------------------------
// load kperf/kperfdata dynamic library
// -----------------------------------------------------------------------------

typedef struct {
  const char *name;
  void **impl;
} lib_symbol;

#define lib_nelems(x) (sizeof(x) / sizeof((x)[0]))
#define lib_symbol_def(name)                                                   \
  { #name, (void *)&name }

static const lib_symbol lib_symbols_kperf[] = {
    lib_symbol_def(kpc_pmu_version),
    lib_symbol_def(kpc_cpu_string),
    lib_symbol_def(kpc_set_counting),
    lib_symbol_def(kpc_get_counting),
    lib_symbol_def(kpc_set_thread_counting),
    lib_symbol_def(kpc_get_thread_counting),
    lib_symbol_def(kpc_get_config_count),
    lib_symbol_def(kpc_get_counter_count),
    lib_symbol_def(kpc_set_config),
    lib_symbol_def(kpc_get_config),
    lib_symbol_def(kpc_get_cpu_counters),
    lib_symbol_def(kpc_get_thread_counters),
    lib_symbol_def(kpc_force_all_ctrs_set),
    lib_symbol_def(kpc_force_all_ctrs_get),
    lib_symbol_def(kperf_action_count_set),
    lib_symbol_def(kperf_action_count_get),
    lib_symbol_def(kperf_action_samplers_set),
    lib_symbol_def(kperf_action_samplers_get),
    lib_symbol_def(kperf_action_filter_set_by_task),
    lib_symbol_def(kperf_action_filter_set_by_pid),
    lib_symbol_def(kperf_timer_count_set),
    lib_symbol_def(kperf_timer_count_get),
    lib_symbol_def(kperf_timer_period_set),
    lib_symbol_def(kperf_timer_period_get),
    lib_symbol_def(kperf_timer_action_set),
    lib_symbol_def(kperf_timer_action_get),
    lib_symbol_def(kperf_sample_set),
    lib_symbol_def(kperf_sample_get),
    lib_symbol_def(kperf_reset),
    lib_symbol_def(kperf_timer_pet_set),
    lib_symbol_def(kperf_timer_pet_get),
    lib_symbol_def(kperf_ns_to_ticks),
    lib_symbol_def(kperf_ticks_to_ns),
    lib_symbol_def(kperf_tick_frequency),
};

static const lib_symbol lib_symbols_kperfdata[] = {
    lib_symbol_def(kpep_config_create),
    lib_symbol_def(kpep_config_free),
    lib_symbol_def(kpep_config_add_event),
    lib_symbol_def(kpep_config_remove_event),
    lib_symbol_def(kpep_config_force_counters),
    lib_symbol_def(kpep_config_events_count),
    lib_symbol_def(kpep_config_events),
    lib_symbol_def(kpep_config_kpc),
    lib_symbol_def(kpep_config_kpc_count),
    lib_symbol_def(kpep_config_kpc_classes),
    lib_symbol_def(kpep_config_kpc_map),
    lib_symbol_def(kpep_db_create),
    lib_symbol_def(kpep_db_free),
    lib_symbol_def(kpep_db_name),
    lib_symbol_def(kpep_db_aliases_count),
    lib_symbol_def(kpep_db_aliases),
    lib_symbol_def(kpep_db_counters_count),
    lib_symbol_def(kpep_db_events_count),
    lib_symbol_def(kpep_db_events),
    lib_symbol_def(kpep_db_event),
    lib_symbol_def(kpep_event_name),
    lib_symbol_def(kpep_event_alias),
    lib_symbol_def(kpep_event_description),
};

#define lib_path_kperf "/System/Library/PrivateFrameworks/kperf.framework/kperf"
#define lib_path_kperfdata                                                     \
  "/System/Library/PrivateFrameworks/kperfdata.framework/kperfdata"

static bool lib_inited = false;
static bool lib_has_err = false;
static char lib_err_msg[256];

static void *lib_handle_kperf = NULL;
static void *lib_handle_kperfdata = NULL;

static void lib_deinit(void) {
  lib_inited = false;
  lib_has_err = false;
  if (lib_handle_kperf)
    dlclose(lib_handle_kperf);
  if (lib_handle_kperfdata)
    dlclose(lib_handle_kperfdata);
  lib_handle_kperf = NULL;
  lib_handle_kperfdata = NULL;
  for (usize i = 0; i < lib_nelems(lib_symbols_kperf); i++) {
    const lib_symbol *symbol = &lib_symbols_kperf[i];
    *symbol->impl = NULL;
  }
  for (usize i = 0; i < lib_nelems(lib_symbols_kperfdata); i++) {
    const lib_symbol *symbol = &lib_symbols_kperfdata[i];
    *symbol->impl = NULL;
  }
}

static bool lib_init(void) {
#define return_err()                                                           \
  do {                                                                         \
    lib_deinit();                                                              \
    lib_inited = true;                                                         \
    lib_has_err = true;                                                        \
    return false;                                                              \
  } while (false)

  if (lib_inited)
    return !lib_has_err;

  // load dynamic library
  lib_handle_kperf = dlopen(lib_path_kperf, RTLD_LAZY);
  if (!lib_handle_kperf) {
    snprintf(lib_err_msg, sizeof(lib_err_msg),
             "Failed to load kperf.framework, message: %s.", dlerror());
    return_err();
  }
  lib_handle_kperfdata = dlopen(lib_path_kperfdata, RTLD_LAZY);
  if (!lib_handle_kperfdata) {
    snprintf(lib_err_msg, sizeof(lib_err_msg),
             "Failed to load kperfdata.framework, message: %s.", dlerror());
    return_err();
  }

  // load symbol address from dynamic library
  for (usize i = 0; i < lib_nelems(lib_symbols_kperf); i++) {
    const lib_symbol *symbol = &lib_symbols_kperf[i];
    *symbol->impl = dlsym(lib_handle_kperf, symbol->name);
    if (!*symbol->impl) {
      snprintf(lib_err_msg, sizeof(lib_err_msg),
               "Failed to load kperf function: %s.", symbol->name);
      return_err();
    }
  }
  for (usize i = 0; i < lib_nelems(lib_symbols_kperfdata); i++) {
    const lib_symbol *symbol = &lib_symbols_kperfdata[i];
    *symbol->impl = dlsym(lib_handle_kperfdata, symbol->name);
    if (!*symbol->impl) {
      snprintf(lib_err_msg, sizeof(lib_err_msg),
               "Failed to load kperfdata function: %s.", symbol->name);
      return_err();
    }
  }

  lib_inited = true;
  lib_has_err = false;
  return true;

#undef return_err
}

// -----------------------------------------------------------------------------
// kdebug private structs
// https://github.com/apple/darwin-xnu/blob/main/bsd/sys_private/kdebug_private.h
// -----------------------------------------------------------------------------

/*
 * Ensure that both LP32 and LP64 variants of arm64 use the same kd_buf
 * structure.
 */
#if defined(__arm64__)
typedef uint64_t kd_buf_argtype;
#else
typedef uintptr_t kd_buf_argtype;
#endif

typedef struct {
  uint64_t timestamp;
  kd_buf_argtype arg1;
  kd_buf_argtype arg2;
  kd_buf_argtype arg3;
  kd_buf_argtype arg4;
  kd_buf_argtype arg5; /* the thread ID */
  uint32_t debugid;    /* see <sys/kdebug.h> */

/*
 * Ensure that both LP32 and LP64 variants of arm64 use the same kd_buf
 * structure.
 */
#if defined(__LP64__) || defined(__arm64__)
  uint32_t cpuid; /* cpu index, from 0 */
  kd_buf_argtype unused;
#endif
} kd_buf;

/* bits for the type field of kd_regtype */
#define KDBG_CLASSTYPE 0x10000
#define KDBG_SUBCLSTYPE 0x20000
#define KDBG_RANGETYPE 0x40000
#define KDBG_TYPENONE 0x80000
#define KDBG_CKTYPES 0xF0000

/* only trace at most 4 types of events, at the code granularity */
#define KDBG_VALCHECK 0x00200000U

typedef struct {
  unsigned int type;
  unsigned int value1;
  unsigned int value2;
  unsigned int value3;
  unsigned int value4;
} kd_regtype;

typedef struct {
  /* number of events that can fit in the buffers */
  int nkdbufs;
  /* set if trace is disabled */
  int nolog;
  /* kd_ctrl_page.flags */
  unsigned int flags;
  /* number of threads in thread map */
  int nkdthreads;
  /* the owning pid */
  int bufid;
} kbufinfo_t;

// -----------------------------------------------------------------------------
// kdebug utils
// -----------------------------------------------------------------------------

/// Clean up trace buffers and reset ktrace/kdebug/kperf.
/// @return 0 on success.
static int kdebug_reset(void) {
  int mib[3] = {CTL_KERN, KERN_KDEBUG, KERN_KDREMOVE};
  return sysctl(mib, 3, NULL, NULL, NULL, 0);
}

/// Disable and reinitialize the trace buffers.
/// @return 0 on success.
static int kdebug_reinit(void) {
  int mib[3] = {CTL_KERN, KERN_KDEBUG, KERN_KDSETUP};
  return sysctl(mib, 3, NULL, NULL, NULL, 0);
}

/// Set debug filter.
static int kdebug_setreg(kd_regtype *kdr) {
  int mib[3] = {CTL_KERN, KERN_KDEBUG, KERN_KDSETREG};
  usize size = sizeof(kd_regtype);
  return sysctl(mib, 3, kdr, &size, NULL, 0);
}

/// Set maximum number of trace entries (kd_buf).
/// Only allow allocation up to half the available memory (sane_size).
/// @return 0 on success.
static int kdebug_trace_setbuf(int nbufs) {
  int mib[4] = {CTL_KERN, KERN_KDEBUG, KERN_KDSETBUF, nbufs};
  return sysctl(mib, 4, NULL, NULL, NULL, 0);
}

/// Enable or disable kdebug trace.
/// Trace buffer must already be initialized.
/// @return 0 on success.
static int kdebug_trace_enable(bool enable) {
  int mib[4] = {CTL_KERN, KERN_KDEBUG, KERN_KDENABLE, enable};
  return sysctl(mib, 4, NULL, 0, NULL, 0);
}

/// Retrieve trace buffer information from kernel.
/// @return 0 on success.
static int kdebug_get_bufinfo(kbufinfo_t *info) {
  if (!info)
    return -1;
  int mib[3] = {CTL_KERN, KERN_KDEBUG, KERN_KDGETBUF};
  size_t needed = sizeof(kbufinfo_t);
  return sysctl(mib, 3, info, &needed, NULL, 0);
}

/// Retrieve trace buffers from kernel.
/// @param buf Memory to receive buffer data, array of `kd_buf`.
/// @param len Length of `buf` in bytes.
/// @param count Number of trace entries (kd_buf) obtained.
/// @return 0 on success.
static int kdebug_trace_read(void *buf, usize len, usize *count) {
  if (count)
    *count = 0;
  if (!buf || !len)
    return -1;

  // Note: the input and output units are not the same.
  // input: bytes
  // output: number of kd_buf
  int mib[3] = {CTL_KERN, KERN_KDEBUG, KERN_KDREADTR};
  int ret = sysctl(mib, 3, buf, &len, NULL, 0);
  if (ret != 0)
    return ret;
  *count = len;
  return 0;
}

/// Block until there are new buffers filled or `timeout_ms` have passed.
/// @param timeout_ms timeout milliseconds, 0 means wait forever.
/// @param suc set true if new buffers filled.
/// @return 0 on success.
static int kdebug_wait(usize timeout_ms, bool *suc) {
  if (timeout_ms == 0)
    return -1;
  int mib[3] = {CTL_KERN, KERN_KDEBUG, KERN_KDBUFWAIT};
  usize val = timeout_ms;
  int ret = sysctl(mib, 3, NULL, &val, NULL, 0);
  if (suc)
    *suc = !!val;
  return ret;
}

// -----------------------------------------------------------------------------
// Demo
// -----------------------------------------------------------------------------

#define EVENT_NAME_MAX 8
typedef struct {
  const char *alias;                 /// name for print
  const char *names[EVENT_NAME_MAX]; /// name from pmc db
} event_alias;

/// Event names from /usr/share/kpep/<name>.plist
static const event_alias profile_events[] = {
    {"cycles",
     {
         "FIXED_CYCLES",            // Apple A7-A15
         "CPU_CLK_UNHALTED.THREAD", // Intel Core 1th-10th
         "CPU_CLK_UNHALTED.CORE",   // Intel Yonah, Merom
     }},
    {"instructions",
     {
         "FIXED_INSTRUCTIONS", // Apple A7-A15
         "INST_RETIRED.ANY"    // Intel Yonah, Merom, Core 1th-10th
     }},
    {"branches",
     {
         "INST_BRANCH",                  // Apple A7-A15
         "BR_INST_RETIRED.ALL_BRANCHES", // Intel Core 1th-10th
         "INST_RETIRED.ANY",             // Intel Yonah, Merom
     }},
    {"branch-misses",
     {
         "BRANCH_MISPRED_NONSPEC",       // Apple A7-A15, since iOS 15, macOS 12
         "BRANCH_MISPREDICT",            // Apple A7-A14
         "BR_MISP_RETIRED.ALL_BRANCHES", // Intel Core 2th-10th
         "BR_INST_RETIRED.MISPRED",      // Intel Yonah, Merom
     }},
};

static kpep_event *get_event(kpep_db *db, const event_alias *alias) {
  for (usize j = 0; j < EVENT_NAME_MAX; j++) {
    const char *name = alias->names[j];
    if (!name)
      break;
    kpep_event *ev = NULL;
    if (kpep_db_event(db, name, &ev) == 0) {
      return ev;
    }
  }
  return NULL;
}

// -----------------------------------------------------------------------------
// Demo 1: profile a function in current thread
// -----------------------------------------------------------------------------

static void profile_func(void) {
  for (u32 i = 0; i < 100000; i++) {
    u32 r = arc4random();
    if (r % 2)
      arc4random();
  }
}

// prepare buffer and config
u32 classes = 0;
usize reg_count = 0;
kpc_config_t regs[KPC_MAX_COUNTERS] = {0};
usize counter_map[KPC_MAX_COUNTERS] = {0};
u64 counters_0[KPC_MAX_COUNTERS] = {0};
u64 counters_1[KPC_MAX_COUNTERS] = {0};
kpep_config *cfg;
kpep_db *db;

const usize ev_count = sizeof(profile_events) / sizeof(profile_events[0]);

kpep_event *ev_arr[ev_count] = {0};

const char *performance_counters_init();
const char *performance_counters_init() {
  int ret = 0;
  // load dylib
  if (!lib_init()) {
    return lib_err_msg;
  }

  // check permission
  int force_ctrs = 0;
  if (kpc_force_all_ctrs_get(&force_ctrs)) {
    return "Permission denied, xnu/kpc requires root privileges.\n";
  }

  // load pmc db
  db = NULL;
  if ((ret = kpep_db_create(NULL, &db))) {
    return "Error: cannot load pmc database";
  }
    // printf("loaded db: %s (%s)\n", db->name, db->marketing_name);
    // printf("number of fixed counters: %zu\n", db->fixed_counter_count);
    // printf("number of configurable counters: %zu\n",
    // db->config_counter_count);

  // create a config
  cfg = NULL;
  if ((ret = kpep_config_create(db, &cfg))) {
    return kpep_config_error_desc(ret);
  }
  if ((ret = kpep_config_force_counters(cfg))) {
    return kpep_config_error_desc(ret);
  }

  // get events

  for (usize i = 0; i < ev_count; i++) {
    const event_alias *alias = profile_events + i;
    ev_arr[i] = get_event(db, alias);
    if (!ev_arr[i]) {
      return alias->alias;
    }
  }

  // add event to config
  for (usize i = 0; i < ev_count; i++) {
    kpep_event *ev = ev_arr[i];
    if ((ret = kpep_config_add_event(cfg, &ev, 0, NULL))) {
      return kpep_config_error_desc(ret);
    }
  }

  if ((ret = kpep_config_kpc_classes(cfg, &classes))) {
    return kpep_config_error_desc(ret);
    // printf("Failed get kpc classes: %d (%s).\n", ret,
    //        kpep_config_error_desc(ret));
    // return 1;
  }
  if ((ret = kpep_config_kpc_count(cfg, &reg_count))) {
    return kpep_config_error_desc(ret);
  }
  if ((ret = kpep_config_kpc_map(cfg, counter_map, sizeof(counter_map)))) {
    return kpep_config_error_desc(ret);
    // return 1;
  }
  if ((ret = kpep_config_kpc(cfg, regs, sizeof(regs)))) {
    return "Failed get kpc registers";
    // printf(": %d (%s).\n", ret,
    //        kpep_config_error_desc(ret));
    // return 1;
  }

  // set config to kernel
  if ((ret = kpc_force_all_ctrs_set(1))) {
    return "Failed force all ctrs";
  }
  if ((classes & KPC_CLASS_CONFIGURABLE_MASK) && reg_count) {
    if ((ret = kpc_set_config(classes, regs))) {
      return "Failed set kpc config";
    }
  }

  return 0;
}

const char *performance_counters_start();
const char *performance_counters_start() {
  int ret = 0;
  // start counting
  if ((ret = kpc_set_counting(classes))) {
    return "Failed set counting";
  }
  if ((ret = kpc_set_thread_counting(classes))) {
    return "Failed set thread counting";
  }

  // get counters before
  if ((ret = kpc_get_thread_counters(0, KPC_MAX_COUNTERS, counters_0))) {
    return "Failed get thread counters before";
  }

  return 0;
}

const char *performance_counters_stop(u64 *values);
const char *performance_counters_stop(u64 *values) {
  int ret = 0;

  // get counters after
  if ((ret = kpc_get_thread_counters(0, KPC_MAX_COUNTERS, counters_1))) {
    return "Failed get thread counters after";
  }

  // stop counting
  kpc_set_counting(0);
  kpc_set_thread_counting(0);
  kpc_force_all_ctrs_set(0);

  for (usize i = 0; i < 4; i++) {
    const event_alias *alias = profile_events + i;
    usize idx = counter_map[i];
    values[i] = counters_1[idx] - counters_0[idx];
  }

  return 0;
}

int main(int argc, const char *argv[]) {
//   int ret = 0;
//   // code to be measured
//   profile_func();

//   // result
//   printf("counters value:\n");

//   // TODO: free memory
  return 0;
}

// -----------------------------------------------------------------------------
// Demo 2: profile a select process
// -----------------------------------------------------------------------------

/// Target process pid, -1 for all thread.
static int target_pid = -1;

/// Profile time in seconds.
static double total_profile_time = 0.1;

/// Profile sampler period in seconds (default 10ms).
static double sample_period = 0.001;

static double get_timestamp(void) {
  struct timeval now;
  gettimeofday(&now, NULL);
  return (double)now.tv_sec + (double)now.tv_usec / (1000.0 * 1000.0);
}

// debugid sub-classes and code from xnu source
#define PERF_KPC (6)
#define PERF_KPC_DATA_THREAD (8)

// int main2(int argc, const char *argv[]) {
//   int ret = 0;

//   // load dylib
//   if (!lib_init()) {
//     printf("Error: %s\n", lib_err_msg);
//     return 1;
//   }

//   // check permission
//   int force_ctrs = 0;
//   if (kpc_force_all_ctrs_get(&force_ctrs)) {
//     return "Permission denied, xnu/kpc requires root privileges.\n");
//     return 1;
//   }

//   // load pmc db
//   kpep_db *db = NULL;
//   if ((ret = kpep_db_create(NULL, &db))) {
//     printf("Error: cannot load pmc database: %d.\n", ret);
//     return 1;
//   }
//   printf("loaded db: %s (%s)\n", db->name, db->marketing_name);
//   printf("number of fixed counters: %zu\n", db->fixed_counter_count);
//   printf("number of configurable counters: %zu\n", db->config_counter_count);
//   printf("CPU tick frequency: %llu\n",
//          (unsigned long long)kperf_tick_frequency());

//   // create a config
//   kpep_config *cfg = NULL;
//   if ((ret = kpep_config_create(db, &cfg))) {
//     printf("Failed to create kpep config: %d (%s).\n", ret,
//            kpep_config_error_desc(ret));
//     return 1;
//   }
//   if ((ret = kpep_config_force_counters(cfg))) {
//     printf("Failed to force counters: %d (%s).\n", ret,
//            kpep_config_error_desc(ret));
//     return 1;
//   }

//   // get events
//   const usize ev_count = sizeof(profile_events) / sizeof(profile_events[0]);
//   kpep_event *ev_arr[ev_count] = {0};
//   for (usize i = 0; i < ev_count; i++) {
//     const event_alias *alias = profile_events + i;
//     ev_arr[i] = get_event(db, alias);
//     if (!ev_arr[i]) {
//       printf("Cannot find event: %s.\n", alias->alias);
//       return 1;
//     }
//   }

//   // add event to config
//   for (usize i = 0; i < ev_count; i++) {
//     kpep_event *ev = ev_arr[i];
//     if ((ret = kpep_config_add_event(cfg, &ev, 0, NULL))) {
//       printf("Failed to add event: %d (%s).\n", ret,
//              kpep_config_error_desc(ret));
//       return 1;
//     }
//   }

//   // prepare buffer and config
//   u32 classes = 0;
//   usize reg_count = 0;
//   kpc_config_t regs[KPC_MAX_COUNTERS] = {0};
//   usize counter_map[KPC_MAX_COUNTERS] = {0};
//   if ((ret = kpep_config_kpc_classes(cfg, &classes))) {
//     printf("Failed get kpc classes: %d (%s).\n", ret,
//            kpep_config_error_desc(ret));
//     return 1;
//   }
//   if ((ret = kpep_config_kpc_count(cfg, &reg_count))) {
//     printf("Failed get kpc count: %d (%s).\n", ret,
//            kpep_config_error_desc(ret));
//     return 1;
//   }
//   if ((ret = kpep_config_kpc_map(cfg, counter_map, sizeof(counter_map)))) {
//     printf("Failed get kpc map: %d (%s).\n", ret, kpep_config_error_desc(ret));
//     return 1;
//   }
//   if ((ret = kpep_config_kpc(cfg, regs, sizeof(regs)))) {
//     printf("Failed get kpc registers: %d (%s).\n", ret,
//            kpep_config_error_desc(ret));
//     return 1;
//   }

//   // set config to kernel
//   if ((ret = kpc_force_all_ctrs_set(1))) {
//     printf("Failed force all ctrs: %d.\n", ret);
//     return 1;
//   }
//   if ((classes & KPC_CLASS_CONFIGURABLE_MASK) && reg_count) {
//     if ((ret = kpc_set_config(classes, regs))) {
//       printf("Failed set kpc config: %d.\n", ret);
//       return 1;
//     }
//   }

//   u32 counter_count = kpc_get_counter_count(classes);
//   if (counter_count == 0) {
//     printf("Failed no counter\n");
//     return 1;
//   }

//   // start counting
//   if ((ret = kpc_set_counting(classes))) {
//     printf("Failed set counting: %d.\n", ret);
//     return 1;
//   }
//   if ((ret = kpc_set_thread_counting(classes))) {
//     printf("Failed set thread counting: %d.\n", ret);
//     return 1;
//   }

//   // action id and timer id
//   u32 actionid = 1;
//   u32 timerid = 1;

//   // alloc action and timer ids
//   if ((ret = kperf_action_count_set(KPERF_ACTION_MAX))) {
//     printf("Failed set action count: %d.\n", ret);
//   }
//   if ((ret = kperf_timer_count_set(KPERF_TIMER_MAX))) {
//     printf("Failed set timer count: %d.\n", ret);
//   }

//   // set what to sample: PMC per thread
//   if ((ret = kperf_action_samplers_set(actionid, KPERF_SAMPLER_PMC_THREAD))) {
//     printf("Failed set sampler type: %d.\n", ret);
//   }
//   // set filter process
//   if ((ret = kperf_action_filter_set_by_pid(actionid, target_pid))) {
//     printf("Failed set filter pid: %d.\n", ret);
//   }

//   // setup PET (Profile Every Thread), start sampler
//   u64 tick = kperf_ns_to_ticks(sample_period * 1000000000ul);
//   if ((ret = kperf_timer_period_set(actionid, tick))) {
//     printf("Failed set timer period: %d.\n", ret);
//   }
//   if ((ret = kperf_timer_action_set(actionid, timerid))) {
//     printf("Failed set timer action: %d.\n", ret);
//   }
//   if ((ret = kperf_timer_pet_set(timerid))) {
//     printf("Failed set timer PET: %d.\n", ret);
//   }
//   if ((ret = kperf_lightweight_pet_set(1))) {
//     printf("Failed set lightweight PET: %d.\n", ret);
//   }
//   if ((ret = kperf_sample_set(1))) {
//     printf("Failed start sample: %d.\n", ret);
//   }

//   // reset kdebug/ktrace
//   if ((ret = kdebug_reset())) {
//     printf("Failed reset kdebug: %d.\n", ret);
//   }

//   int nbufs = 1000000;
//   if ((ret = kdebug_trace_setbuf(nbufs))) {
//     printf("Failed setbuf: %d.\n", ret);
//   }
//   if ((ret = kdebug_reinit())) {
//     printf("Failed init kdebug buffer: %d.\n", ret);
//   }

//   // set trace filter: only log PERF_KPC_DATA_THREAD
//   kd_regtype kdr = {0};
//   kdr.type = KDBG_VALCHECK;
//   kdr.value1 = KDBG_EVENTID(DBG_PERF, PERF_KPC, PERF_KPC_DATA_THREAD);
//   if ((ret = kdebug_setreg(&kdr))) {
//     printf("Failed set kdebug filter: %d.\n", ret);
//   }
//   // start trace
//   if ((ret = kdebug_trace_enable(1))) {
//     printf("Failed enable kdebug trace: %d.\n", ret);
//   }

//   // sample and get buffers
//   usize buf_capacity = nbufs * 2;
//   kd_buf *buf_hdr = malloc(sizeof(kd_buf) * buf_capacity);
//   kd_buf *buf_cur = buf_hdr;
//   kd_buf *buf_end = buf_hdr + buf_capacity;

//   double begin = get_timestamp();
//   while (buf_hdr) {
//     // wait for more buffer
//     usleep(2 * sample_period * 1000000);

//     // expand local buffer for next read
//     if (buf_end - buf_cur < nbufs) {
//       usize new_capacity = buf_capacity * 2;
//       kd_buf *new_buf = realloc(buf_hdr, sizeof(kd_buf) * new_capacity);
//       if (!new_buf) {
//         free(buf_hdr);
//         buf_hdr = NULL;
//         break;
//       }
//       buf_capacity = new_capacity;
//       buf_cur = new_buf + (buf_cur - buf_hdr);
//       buf_end = new_buf + (buf_end - buf_hdr);
//       buf_hdr = new_buf;
//     }

//     // read trace buffer from kernel
//     usize count = 0;
//     kdebug_trace_read(buf_cur, sizeof(kd_buf) * nbufs, &count);
//     for (kd_buf *buf = buf_cur, *end = buf_cur + count; buf < end; buf++) {
//       u32 debugid = buf->debugid;
//       u32 cls = KDBG_EXTRACT_CLASS(debugid);
//       u32 subcls = KDBG_EXTRACT_SUBCLASS(debugid);
//       u32 code = KDBG_EXTRACT_CODE(debugid);

//       // keep only thread PMC data
//       if (cls != DBG_PERF)
//         continue;
//       if (subcls != PERF_KPC)
//         continue;
//       if (code != PERF_KPC_DATA_THREAD)
//         continue;
//       memmove(buf_cur, buf, sizeof(kd_buf));
//       buf_cur++;
//     }

//     // stop when time is up
//     double now = get_timestamp();
//     if (now - begin > total_profile_time + sample_period)
//       break;
//   }

//   // stop tracing
//   kdebug_trace_enable(0);
//   kdebug_reset();
//   kperf_sample_set(0);
//   kperf_lightweight_pet_set(0);

//   // stop counting
//   kpc_set_counting(0);
//   kpc_set_thread_counting(0);
//   kpc_force_all_ctrs_set(0);

//   // aggregate thread PMC data
//   if (!buf_hdr) {
//     printf("Failed to allocate memory for trace log.\n");
//     return 1;
//   }
//   if (buf_cur - buf_hdr == 0) {
//     printf("No thread PMC data collected.\n");
//     return 1;
//   }

//   typedef struct {
//     u32 tid;
//     u64 timestamp_0;
//     u64 timestamp_1;
//     u64 counters_0[KPC_MAX_COUNTERS];
//     u64 counters_1[KPC_MAX_COUNTERS];
//   } kpc_thread_data;

//   usize thread_capacity = 16;
//   usize thread_count = 0;
//   kpc_thread_data *thread_data =
//       malloc(thread_capacity * sizeof(kpc_thread_data));
//   if (!thread_data) {
//     printf("Failed to allocate memory for aggregate log.\n");
//     return 1;
//   }
//   for (kd_buf *buf = buf_hdr; buf < buf_cur; buf++) {
//     u32 func = buf->debugid & KDBG_FUNC_MASK;
//     if (func != DBG_FUNC_START)
//       continue;
//     u32 tid = (u32)buf->arg5;
//     if (!tid)
//       continue;

//     // read one counter log
//     u32 ci = 0;
//     u64 counters[KPC_MAX_COUNTERS];
//     counters[ci++] = buf->arg1;
//     counters[ci++] = buf->arg2;
//     counters[ci++] = buf->arg3;
//     counters[ci++] = buf->arg4;
//     if (ci < counter_count) {
//       // counter count larger than 4
//       // values are split into multiple buffer entities
//       for (kd_buf *buf2 = buf + 1; buf2 < buf_cur; buf2++) {
//         u32 tid2 = (u32)buf2->arg5;
//         if (tid2 != tid)
//           break;
//         u32 func2 = buf2->debugid & KDBG_FUNC_MASK;
//         if (func2 == DBG_FUNC_START)
//           break;
//         if (ci < counter_count)
//           counters[ci++] = buf2->arg1;
//         if (ci < counter_count)
//           counters[ci++] = buf2->arg2;
//         if (ci < counter_count)
//           counters[ci++] = buf2->arg3;
//         if (ci < counter_count)
//           counters[ci++] = buf2->arg4;
//         if (ci == counter_count)
//           break;
//       }
//     }
//     if (ci != counter_count)
//       continue; // not enough counters, maybe truncated

//     // add to thread data
//     kpc_thread_data *data = NULL;
//     for (usize i = 0; i < thread_count; i++) {
//       if (thread_data[i].tid == tid) {
//         data = thread_data + i;
//         break;
//       }
//     }
//     if (!data) {
//       if (thread_capacity == thread_count) {
//         thread_capacity *= 2;
//         kpc_thread_data *new_data =
//             realloc(thread_data, thread_capacity * sizeof(kpc_thread_data));
//         if (!new_data) {
//           printf("Failed to allocate memory for aggregate log.\n");
//           return 1;
//         }
//         thread_data = new_data;
//       }
//       data = thread_data + thread_count;
//       thread_count++;
//       memset(data, 0, sizeof(kpc_thread_data));
//       data->tid = tid;
//     }
//     if (data->timestamp_0 == 0) {
//       data->timestamp_0 = buf->timestamp;
//       memcpy(data->counters_0, counters, counter_count * sizeof(u64));
//     } else {
//       data->timestamp_1 = buf->timestamp;
//       memcpy(data->counters_1, counters, counter_count * sizeof(u64));
//     }
//   }

//   u64 counters_sum[KPC_MAX_COUNTERS] = {0};
//   for (usize i = 0; i < thread_count; i++) {
//     kpc_thread_data *data = thread_data + i;
//     if (!data->timestamp_0 || !data->timestamp_1)
//       continue;

//     u64 counters_one[KPC_MAX_COUNTERS] = {0};
//     for (usize c = 0; c < counter_count; c++) {
//       counters_one[c] += data->counters_1[c] - data->counters_0[c];
//     }
//     printf("------------------------\n");
//     printf("thread: %u, trace time: %f\n", data->tid,
//            kperf_ticks_to_ns(data->timestamp_1 - data->timestamp_0) /
//                1000000000.0);
//     for (usize i = 0; i < ev_count; i++) {
//       const event_alias *alias = profile_events + i;
//       u64 val = counters_one[counter_map[i]];
//       printf("%14s: %llu\n", alias->alias, val);
//     }

//     for (usize c = 0; c < counter_count; c++) {
//       counters_sum[c] += counters_one[c];
//     }
//   }

//   printf("------------------------\n");
//   printf("all threads:\n");
//   for (usize i = 0; i < ev_count; i++) {
//     const event_alias *alias = profile_events + i;
//     u64 val = counters_sum[counter_map[i]];
//     printf("%14s: %llu\n", alias->alias, val);
//   }

//   // TODO: free memory
//   return 0;
// }