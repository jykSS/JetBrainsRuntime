/*
 * Copyright (c) 2013, 2018, Red Hat, Inc. and/or its affiliates.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#include "precompiled.hpp"
#include "gc/shared/gcTraceTime.inline.hpp"
#include "gc/shenandoah/shenandoahConcurrentMark.inline.hpp"
#include "gc/shenandoah/shenandoahCollectorPolicy.hpp"
#include "gc/shenandoah/shenandoahFreeSet.hpp"
#include "gc/shenandoah/shenandoahPhaseTimings.hpp"
#include "gc/shenandoah/shenandoahHeap.inline.hpp"
#include "gc/shenandoah/shenandoahHeuristics.hpp"
#include "gc/shenandoah/shenandoahMonitoringSupport.hpp"
#include "gc/shenandoah/shenandoahControlThread.hpp"
#include "gc/shenandoah/shenandoahTraversalGC.hpp"
#include "gc/shenandoah/shenandoahUtils.hpp"
#include "gc/shenandoah/shenandoahWorkerPolicy.hpp"
#include "gc/shenandoah/vm_operations_shenandoah.hpp"
#include "memory/iterator.hpp"
#include "memory/universe.hpp"
#include "runtime/vmThread.hpp"

ShenandoahControlThread::ShenandoahControlThread() :
  ConcurrentGCThread(),
  _alloc_failure_waiters_lock(Mutex::leaf, "ShenandoahAllocFailureGC_lock", true, Monitor::_safepoint_check_always),
  _explicit_gc_waiters_lock(Mutex::leaf, "ShenandoahExplicitGC_lock", true, Monitor::_safepoint_check_always),
  _periodic_task(this),
  _explicit_gc_cause(GCCause::_no_cause_specified),
  _degen_point(ShenandoahHeap::_degenerated_outside_cycle),
  _allocs_seen(0)
{
  create_and_start();
  _periodic_task.enroll();
}

ShenandoahControlThread::~ShenandoahControlThread() {
  // This is here so that super is called.
}

void ShenandoahPeriodicTask::task() {
  _thread->handle_force_counters_update();
  _thread->handle_counters_update();
}

void ShenandoahControlThread::run_service() {
  ShenandoahHeap* heap = ShenandoahHeap::heap();

  int sleep = ShenandoahControlIntervalMin;

  double last_shrink_time = os::elapsedTime();
  double last_sleep_adjust_time = os::elapsedTime();

  // Shrink period avoids constantly polling regions for shrinking.
  // Having a period 10x lower than the delay would mean we hit the
  // shrinking with lag of less than 1/10-th of true delay.
  // ShenandoahUncommitDelay is in msecs, but shrink_period is in seconds.
  double shrink_period = (double)ShenandoahUncommitDelay / 1000 / 10;

  ShenandoahCollectorPolicy* policy = heap->shenandoahPolicy();
  ShenandoahHeuristics* heuristics = heap->heuristics();
  while (!in_graceful_shutdown() && !should_terminate()) {
    // Figure out if we have pending requests.
    bool alloc_failure_pending = _alloc_failure_gc.is_set();
    bool explicit_gc_requested = _explicit_gc.is_set();

    // This control loop iteration have seen this much allocations.
    size_t allocs_seen = Atomic::xchg<size_t>(0, &_allocs_seen);

    // Choose which GC mode to run in. The block below should select a single mode.
    GCMode mode = none;
    GCCause::Cause cause = GCCause::_last_gc_cause;
    ShenandoahHeap::ShenandoahDegenPoint degen_point = ShenandoahHeap::_degenerated_unset;

    if (alloc_failure_pending) {
      // Allocation failure takes precedence: we have to deal with it first thing
      cause = GCCause::_allocation_failure;

      // Consume the degen point, and seed it with default value
      degen_point = _degen_point;
      _degen_point = ShenandoahHeap::_degenerated_outside_cycle;

      if (ShenandoahDegeneratedGC && heuristics->should_degenerate_cycle()) {
        heuristics->record_allocation_failure_gc();
        policy->record_alloc_failure_to_degenerated(degen_point);
        mode = stw_degenerated;
      } else {
        heuristics->record_allocation_failure_gc();
        policy->record_alloc_failure_to_full();
        mode = stw_full;
      }

    } else if (explicit_gc_requested) {
      // Honor explicit GC requests
      if (ExplicitGCInvokesConcurrent) {
        heuristics->record_explicit_gc();
        policy->record_explicit_to_concurrent();
        if (heuristics->can_do_traversal_gc()) {
          mode = concurrent_traversal;
        } else {
          mode = concurrent_normal;
        }
      } else {
        heuristics->record_explicit_gc();
        policy->record_explicit_to_full();
        mode = stw_full;
      }
      cause = _explicit_gc_cause;
    } else {
      // Potential normal cycle: ask heuristics if it wants to act
      ShenandoahHeap::GCCycleMode traversal_mode = heuristics->should_start_traversal_gc();
      if (traversal_mode != ShenandoahHeap::NONE) {
        mode = concurrent_traversal;
        cause = GCCause::_shenandoah_traversal_gc;
        heap->set_cycle_mode(traversal_mode);
      } else if (heuristics->should_start_normal_gc()) {
        mode = concurrent_normal;
        cause = GCCause::_shenandoah_concurrent_gc;
        heap->set_cycle_mode(ShenandoahHeap::MAJOR);
      }

      // Ask policy if this cycle wants to process references or unload classes
      heap->set_process_references(heuristics->should_process_references());
      heap->set_unload_classes(heuristics->should_unload_classes());
    }

    bool gc_requested = (mode != none);
    assert (!gc_requested || cause != GCCause::_last_gc_cause, "GC cause should be set");

    if (gc_requested) {
      heap->reset_bytes_allocated_since_gc_start();

      // If GC was requested, we are sampling the counters even without actual triggers
      // from allocation machinery. This captures GC phases more accurately.
      set_forced_counters_update(true);

      // If GC was requested, we better dump freeset data for performance debugging
      {
        ShenandoahHeapLocker locker(heap->lock());
        heap->free_set()->log_status_verbose();
      }
    }

    switch (mode) {
      case none:
        break;
      case concurrent_traversal:
        service_concurrent_traversal_cycle(cause);
        break;
      case concurrent_normal:
        service_concurrent_normal_cycle(cause);
        break;
      case stw_degenerated:
        service_stw_degenerated_cycle(cause, degen_point);
        break;
      case stw_full:
        service_stw_full_cycle(cause);
        break;
      default:
        ShouldNotReachHere();
    }

    heap->set_cycle_mode(ShenandoahHeap::NONE);

    if (gc_requested) {
      heap->set_used_at_last_gc();

      // If this was the explicit GC cycle, notify waiters about it
      if (explicit_gc_requested) {
        notify_explicit_gc_waiters();

        // Explicit GC tries to uncommit everything
        heap->handle_heap_shrinkage(os::elapsedTime());
      }

      // If this was the allocation failure GC cycle, notify waiters about it
      if (alloc_failure_pending) {
        notify_alloc_failure_waiters();
      }

      // Report current free set state at the end of cycle, whether
      // it is a normal completion, or the abort.
      {
        ShenandoahHeapLocker locker(heap->lock());
        heap->free_set()->log_status_verbose();
      }

      // Disable forced counters update, and update counters one more time
      // to capture the state at the end of GC session.
      handle_force_counters_update();
      set_forced_counters_update(false);

      // GC is over, we are at idle now
      if (ShenandoahPacing) {
        heap->pacer()->setup_for_idle();
      }
    } else {
      // Allow allocators to know we have seen this much regions
      if (ShenandoahPacing && (allocs_seen > 0)) {
        heap->pacer()->report_alloc(allocs_seen);
      }
    }

    double current = os::elapsedTime();

    // Try to uncommit stale regions
    if (current - last_shrink_time > shrink_period) {
      heap->handle_heap_shrinkage(current - (ShenandoahUncommitDelay / 1000.0));
      last_shrink_time = current;
    }

    // Wait before performing the next action. If allocation happened during this wait,
    // we exit sooner, to let heuristics re-evaluate new conditions. If we are at idle,
    // back off exponentially.
    if (_heap_changed.try_unset()) {
      sleep = ShenandoahControlIntervalMin;
    } else if ((current - last_sleep_adjust_time) * 1000 > ShenandoahControlIntervalAdjustPeriod){
      sleep = MIN2<int>(ShenandoahControlIntervalMax, MAX2(1, sleep * 2));
      last_sleep_adjust_time = current;
    }
    os::naked_short_sleep(sleep);
  }

  // Wait for the actual stop(), can't leave run_service() earlier.
  while (!should_terminate()) {
    os::naked_short_sleep(ShenandoahControlIntervalMin);
  }
}

void ShenandoahControlThread::service_concurrent_traversal_cycle(GCCause::Cause cause) {
  GCIdMark gc_id_mark;
  ShenandoahGCSession session;

  ShenandoahHeap* heap = ShenandoahHeap::heap();
  bool is_minor = heap->is_minor_gc();
  TraceCollectorStats tcs(is_minor ? heap->monitoring_support()->partial_collection_counters()
                                   : heap->monitoring_support()->concurrent_collection_counters());

  heap->vmop_entry_init_traversal();

  if (check_cancellation_or_degen(ShenandoahHeap::_degenerated_traversal)) return;

  heap->entry_traversal();
  if (check_cancellation_or_degen(ShenandoahHeap::_degenerated_traversal)) return;

  heap->vmop_entry_final_traversal();

  heap->entry_cleanup_traversal();

  heap->heuristics()->record_success_concurrent();
  heap->shenandoahPolicy()->record_success_concurrent();
}

void ShenandoahControlThread::service_concurrent_normal_cycle(GCCause::Cause cause) {
  // Normal cycle goes via all concurrent phases. If allocation failure (af) happens during
  // any of the concurrent phases, it first degrades to Degenerated GC and completes GC there.
  // If second allocation failure happens during Degenerated GC cycle (for example, when GC
  // tries to evac something and no memory is available), cycle degrades to Full GC.
  //
  // The only current exception is allocation failure in Conc Evac: it goes straight to Full GC,
  // because we don't recover well from the case of incompletely evacuated heap in STW cycle.
  //
  // There are also two shortcuts through the normal cycle: a) immediate garbage shortcut, when
  // heuristics says there are no regions to compact, and all the collection comes from immediately
  // reclaimable regions; b) coalesced UR shortcut, when heuristics decides to coalesce UR with the
  // mark from the next cycle.
  //
  // ................................................................................................
  //
  //                                    (immediate garbage shortcut)                Concurrent GC
  //                             /-------------------------------------------\
  //                             |                       (coalesced UR)      v
  //                             |                  /----------------------->o
  //                             |                  |                        |
  //                             |                  |                        v
  // [START] ----> Conc Mark ----o----> Conc Evac --o--> Conc Update-Refs ---o----> [END]
  //                   |                    |                 |              ^
  //                   | (af)               | (af)            | (af)         |
  // ..................|....................|.................|..............|.......................
  //                   |                    |                 |              |
  //                   |          /---------/                 |              |      Degenerated GC
  //                   v          |                           v              |
  //               STW Mark ------+---> STW Evac ----> STW Update-Refs ----->o
  //                   |          |         |                 |              ^
  //                   | (af)     |         | (af)            | (af)         |
  // ..................|..........|.........|.................|..............|.......................
  //                   |          |         |                 |              |
  //                   |          v         v                 |              |      Full GC
  //                   \--------->o-------->o<----------------/              |
  //                                        |                                |
  //                                        v                                |
  //                                      Full GC  --------------------------/
  //
  ShenandoahHeap* heap = ShenandoahHeap::heap();

  if (check_cancellation_or_degen(ShenandoahHeap::_degenerated_outside_cycle)) return;

  GCIdMark gc_id_mark;
  ShenandoahGCSession session;

  // Capture peak occupancy right after starting the cycle
  heap->heuristics()->record_peak_occupancy();

  TraceCollectorStats tcs(heap->monitoring_support()->concurrent_collection_counters());

  // Start initial mark under STW
  heap->vmop_entry_init_mark();

  // Continue concurrent mark
  heap->entry_mark();
  if (check_cancellation_or_degen(ShenandoahHeap::_degenerated_mark)) return;

  // If not cancelled, can try to concurrently pre-clean
  heap->entry_preclean();

  // Complete marking under STW, and start evacuation
  heap->vmop_entry_final_mark();

  // Continue the cycle with evacuation and optional update-refs.
  // This may be skipped if there is nothing to evacuate.
  // If so, evac_in_progress would be unset by collection set preparation code.
  if (heap->is_evacuation_in_progress()) {
    // Final mark had reclaimed some immediate garbage, kick cleanup to reclaim the space
    // for the rest of the cycle.
    heap->entry_cleanup();

    // Concurrently evacuate
    heap->entry_evac();
    if (check_cancellation_or_degen(ShenandoahHeap::_degenerated_evac)) return;

    // Perform update-refs phase, if required. This phase can be skipped if heuristics
    // decides to piggy-back the update-refs on the next marking cycle. On either path,
    // we need to turn off evacuation: either in init-update-refs, or in final-evac.
    if (heap->heuristics()->should_start_update_refs()) {
      heap->vmop_entry_init_updaterefs();
      heap->entry_updaterefs();
      if (check_cancellation_or_degen(ShenandoahHeap::_degenerated_updaterefs)) return;

      heap->vmop_entry_final_updaterefs();
    } else {
      heap->vmop_entry_final_evac();
    }
  }

  // Reclaim space and prepare for the next normal cycle:
  heap->entry_cleanup_bitmaps();

  // Cycle is complete
  heap->heuristics()->record_success_concurrent();
  heap->shenandoahPolicy()->record_success_concurrent();
}

bool ShenandoahControlThread::check_cancellation_or_degen(ShenandoahHeap::ShenandoahDegenPoint point) {
  ShenandoahHeap* heap = ShenandoahHeap::heap();
  if (heap->cancelled_gc()) {
    assert (is_alloc_failure_gc() || in_graceful_shutdown(), "Cancel GC either for alloc failure GC, or gracefully exiting");
    if (!in_graceful_shutdown()) {
      assert (_degen_point == ShenandoahHeap::_degenerated_outside_cycle,
              "Should not be set yet: %s", ShenandoahHeap::degen_point_to_string(_degen_point));
      _degen_point = point;
    }
    return true;
  }
  return false;
}

void ShenandoahControlThread::stop_service() {
  // Nothing to do here.
}

void ShenandoahControlThread::service_stw_full_cycle(GCCause::Cause cause) {
  GCIdMark gc_id_mark;
  ShenandoahGCSession session;

  ShenandoahHeap* heap = ShenandoahHeap::heap();
  heap->vmop_entry_full(cause);

  heap->heuristics()->record_success_full();
  heap->shenandoahPolicy()->record_success_full();
}

void ShenandoahControlThread::service_stw_degenerated_cycle(GCCause::Cause cause, ShenandoahHeap::ShenandoahDegenPoint point) {
  assert (point != ShenandoahHeap::_degenerated_unset, "Degenerated point should be set");

  GCIdMark gc_id_mark;
  ShenandoahGCSession session;

  ShenandoahHeap* heap = ShenandoahHeap::heap();
  heap->vmop_degenerated(point);

  heap->heuristics()->record_success_degenerated();
  heap->shenandoahPolicy()->record_success_degenerated();
}

void ShenandoahControlThread::handle_explicit_gc(GCCause::Cause cause) {
  assert(GCCause::is_user_requested_gc(cause) ||
         GCCause::is_serviceability_requested_gc(cause) ||
         cause == GCCause::_full_gc_alot ||
         cause == GCCause::_wb_full_gc ||
         cause == GCCause::_scavenge_alot,
         "only requested GCs here");
  if (!DisableExplicitGC) {
    _explicit_gc_cause = cause;

    _explicit_gc.set();
    MonitorLockerEx ml(&_explicit_gc_waiters_lock);
    while (_explicit_gc.is_set()) {
      ml.wait();
    }
  }
}

void ShenandoahControlThread::handle_alloc_failure(size_t words) {
  ShenandoahHeap* heap = ShenandoahHeap::heap();

  heap->soft_ref_policy()->set_should_clear_all_soft_refs(true);
  assert(current()->is_Java_thread(), "expect Java thread here");

  if (try_set_alloc_failure_gc()) {
    // Only report the first allocation failure
    log_info(gc)("Failed to allocate " SIZE_FORMAT "K", words * HeapWordSize / K);

    // Now that alloc failure GC is scheduled, we can abort everything else
    heap->cancel_gc(GCCause::_allocation_failure);
  }

  MonitorLockerEx ml(&_alloc_failure_waiters_lock);
  while (is_alloc_failure_gc()) {
    ml.wait();
  }
}

void ShenandoahControlThread::handle_alloc_failure_evac(size_t words) {
  log_develop_trace(gc)("Out of memory during evacuation, cancel evacuation, schedule GC by thread %d",
                        Thread::current()->osthread()->thread_id());

  ShenandoahHeap* heap = ShenandoahHeap::heap();
  heap->soft_ref_policy()->set_should_clear_all_soft_refs(true);

  if (try_set_alloc_failure_gc()) {
    // Only report the first allocation failure
    log_info(gc)("Failed to allocate " SIZE_FORMAT "K for evacuation", words * HeapWordSize / K);
  }

  // Forcefully report allocation failure
  heap->cancel_gc(GCCause::_shenandoah_allocation_failure_evac);
}

void ShenandoahControlThread::notify_alloc_failure_waiters() {
  _alloc_failure_gc.unset();
  MonitorLockerEx ml(&_alloc_failure_waiters_lock);
  ml.notify_all();
}

bool ShenandoahControlThread::try_set_alloc_failure_gc() {
  return _alloc_failure_gc.try_set();
}

bool ShenandoahControlThread::is_alloc_failure_gc() {
  return _alloc_failure_gc.is_set();
}

void ShenandoahControlThread::notify_explicit_gc_waiters() {
  _explicit_gc.unset();
  MonitorLockerEx ml(&_explicit_gc_waiters_lock);
  ml.notify_all();
}

void ShenandoahControlThread::handle_counters_update() {
  if (_do_counters_update.is_set()) {
    _do_counters_update.unset();
    ShenandoahHeap::heap()->monitoring_support()->update_counters();
  }
}

void ShenandoahControlThread::handle_force_counters_update() {
  if (_force_counters_update.is_set()) {
    _do_counters_update.unset(); // reset these too, we do update now!
    ShenandoahHeap::heap()->monitoring_support()->update_counters();
  }
}

void ShenandoahControlThread::notify_heap_changed() {
  // This is called from allocation path, and thus should be fast.

  // Update monitoring counters when we took a new region. This amortizes the
  // update costs on slow path.
  if (_do_counters_update.is_unset()) {
    _do_counters_update.set();
  }
  // Notify that something had changed.
  if (_heap_changed.is_unset()) {
    _heap_changed.set();
  }
}

void ShenandoahControlThread::pacing_notify_alloc(size_t words) {
  assert(ShenandoahPacing, "should only call when pacing is enabled");
  Atomic::add(words, &_allocs_seen);
}

void ShenandoahControlThread::set_forced_counters_update(bool value) {
  _force_counters_update.set_cond(value);
}

void ShenandoahControlThread::print() const {
  print_on(tty);
}

void ShenandoahControlThread::print_on(outputStream* st) const {
  st->print("Shenandoah Concurrent Thread");
  Thread::print_on(st);
  st->cr();
}

void ShenandoahControlThread::start() {
  create_and_start();
}

void ShenandoahControlThread::prepare_for_graceful_shutdown() {
  _graceful_shutdown.set();
}

bool ShenandoahControlThread::in_graceful_shutdown() {
  return _graceful_shutdown.is_set();
}
