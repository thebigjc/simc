// ==========================================================================
// Dedmonwakeen's Raid DPS/TPS Simulator.
// Send questions to natehieter@gmail.com
// ==========================================================================

#include "profileset.hpp"
#include "dbc/dbc.hpp"
#include "sim_control.hpp"
#include "sim.hpp"
#include "report/reports.hpp"
#include "player/player.hpp"
#include "player/player_talent_points.hpp"
#include "item/item.hpp"
#include "util/string_view.hpp"

#ifndef SC_NO_THREADING

#include <future>
#include <iostream>
#include <memory>
#include <numeric>
#include <set>
#include <sstream>

namespace
{
// Options that allow "appending" or "mapping" cannot really be overridden in the current
// climate. Note that this does not take into account class module options, and there's no
// easy way to do that, short of iterating over defined option parsers every single time.
// The simc option system really needs the "scoping" system fleshed out to understand
// where a specific option (i.e., a name, value tuple) is defined.
//
// Also, any "function" option is going to be undefined behavior whether it can be merged
// or not, but typically the function option semantics work as such that the next parsing
// of the same option will override whatever is being done.
bool overridable_option( const option_tuple_t& tuple )
{
  return tuple.value.rfind( '=' ) == std::string::npos &&
         tuple.name.rfind( "actions", 0 ) == std::string::npos &&
         tuple.name.rfind( "items", 0 ) == std::string::npos &&
         tuple.name.rfind( "raid_events", 0 ) == std::string::npos &&
         tuple.name.rfind( "class_talents", 0 ) == std::string::npos &&
         tuple.name.rfind( "spec_talents", 0 ) == std::string::npos &&
         tuple.name.rfind( "hero_talents", 0 ) == std::string::npos;
}

std::string format_time( double seconds, bool milliseconds = true )
{
  std::stringstream s;

  if ( seconds == 0 )
  {
    return "0s";
  }
  // For milliseconds, just use a quick format
  else if ( seconds < 1 )
  {
    s << util::round( 1000.0 * seconds, 3 ) << "ms";
  }
  // Otherwise, do the whole thing
  else
  {
    int days = 0;
    int hours = 0;
    int minutes = 0;

    double remainder = seconds;

    if ( remainder >= 86400 )
    {
      days = static_cast<int>(remainder / 86400);
      remainder -= days * 86400;
    }

    if ( remainder >= 3600 )
    {
      hours = static_cast<int>(remainder / 3600);
      remainder -= hours * 3600;
    }

    if ( remainder >= 60 )
    {
      minutes = static_cast<int>(remainder / 60);
      remainder -= minutes * 60;
    }

    if ( days > 0 )
    {
      s << days << "d, ";
    }

    if ( hours > 0 )
    {
      s << hours << "h, ";
    }

    if ( minutes > 0 )
    {
      s << minutes << "m, ";
    }

    s << util::round( remainder, milliseconds ? 3 : 0 ) << "s";
  }

  return s.str();
}

// Deallocating profile_sim is the responsibility of the caller (i.e., profileset driver or
// worker_t)
void simulate_profileset( sim_t* parent, profileset::profile_set_t& set, sim_t*& profile_sim )
{
  // Reset random seed for the profileset sims
  profile_sim -> seed = 0;
  profile_sim -> profileset_enabled = true;
  profile_sim -> profileset_current_name = set.name();
  profile_sim -> report_details = 0;
  if ( parent -> profileset_work_threads > 0 )
  {
    profile_sim -> threads = parent -> profileset_work_threads;
    // Disable reporting on parallel sims, instead, rely on parallel sims finishing to report
    // progress. For normal profileset simming we can rely on the normal progressbar updates
    profile_sim -> report_progress = false;
  }
  else
  {
    profile_sim -> progress_bar.set_base( "Profileset" );
    profile_sim -> progress_bar.set_phase( set.name() );
  }

  if ( parent->profileset_cull.enabled &&
       parent->profileset_cull.method == sim_t::profileset_cull_state_t::CONFSEQ_TOTAL_ORDER )
  {
    auto& cull = parent->profileset_cull;
    int chunk = cull.chunk_iterations();
    if ( chunk <= 0 )
      chunk = cull.min_iterations;

    // Default target is one chunk; will be overridden by per-arm plan if present.
    int target_iterations = std::min( chunk, parent->iterations );
    int remaining_for_arm = parent->iterations;

    {
      std::lock_guard<mutex_t> guard( cull.mtx );
      if ( const auto* arm = cull.find_arm( set.name() ) )
      {
        int remaining = parent->iterations - arm->iterations;
        remaining_for_arm = remaining;
        if ( remaining > 0 )
        {
            target_iterations = std::min( chunk, remaining );
          }
        else
        {
          target_iterations = 0;
        }
      }
    }

    if ( target_iterations <= 0 )
    {
      if ( cull.verbose >= 2 )
      {
        fmt::print( stderr,
                    "profileset_cull: refinement skip '{}' (no remaining budget)\n",
                    set.name() );
      }
      // Nothing to simulate for this arm; exit early.
      return;
    }

    profile_sim->iterations = target_iterations;

    if ( cull.verbose >= 2 )
    {
      fmt::print( stderr,
                  "profileset_cull: refinement '{}': {} iterations (remaining {})\n",
                  set.name(), target_iterations, remaining_for_arm );
    }
  }

  auto ret = profile_sim -> execute();
  if ( ret )
  {
    profile_sim -> progress_bar.restart();

    if ( set.has_output() )
    {
      report::print_suite( profile_sim );
    }
  }

  if ( !ret || profile_sim -> is_canceled() )
  {
    return;
  }

  const auto player = profile_sim -> player_no_pet_list[ parent->profileset_report_player_index ];
  auto progress = profile_sim -> progress( nullptr, 0 );

  range::for_each( parent -> profileset_metric, [ & ]( scale_metric_e metric ) {
    auto data = profileset::metric_data( player, metric );

    set.result( metric )
      .min( data.min )
      .first_quartile( data.first_quartile )
      .median( data.median )
      .mean( data.mean )
      .third_quartile( data.third_quartile )
      .max( data.max )
      .stddev( data.std_dev )
      .mean_stddev( data.mean_std_dev )
      .iterations( progress.current_iterations );

    // If culled, persist snapshot information for JSON/HTML reporting on primary metric only
    if ( profile_sim->culled && metric == parent->profileset_metric.front() )
    {
    // error to record depends on method: CI mode wants half-width, t-test wants SE
      auto etype = ( parent->profileset_cull.prefers_standard_error() ) ?
        profileset::profile_set_t::cull_error_type_e::STANDARD_ERROR :
        profileset::profile_set_t::cull_error_type_e::CI_HALF_WIDTH;
      double err_val = parent->profileset_cull.select_error(data.mean_std_dev * parent->confidence_estimator, data.mean_std_dev / sqrt(parent->iterations) );
      set.set_culled( true,
                      profile_sim->culled_reason,
                      progress.current_iterations,
                      data.mean,
                      err_val,
                      etype );
    }
  } );

  if ( ! parent -> profileset_output_data.empty() )
  {
    const auto parent_player = parent -> player_no_pet_list[ parent->profileset_report_player_index ];
    range::for_each( parent -> profileset_output_data, [ & ]( const std::string& option ) {
        save_output_data( set, parent_player, player, option );
    } );
  }

  // Save global statistics back to parent sim
  parent -> elapsed_cpu  += profile_sim -> elapsed_cpu;
  parent -> init_time    += profile_sim -> init_time;
  parent -> merge_time   += profile_sim -> merge_time;
  parent -> analyze_time += profile_sim -> analyze_time;
  parent -> event_mgr.total_events_processed += profile_sim -> event_mgr.total_events_processed;

  if ( profile_sim->culled )
  {
    fmt::print( stderr, "\nProfileset '{}' culled: {}\n", set.name(), profile_sim->culled_reason );
  }
  else if ( profile_sim->futile )
  {
    fmt::print( stderr, "\nProfileset '{}' stopped early: {}\n", set.name(), profile_sim->culled_reason );
  }
}

// Figure out if the option defines new actor(s) with their own scope
bool is_actor_scope( const option_tuple_t& opt )
{
  static constexpr std::array<util::string_view, 22> actor_scope_opts { {
    "deathknight", "demonhunter", "druid", "evoker", "hunter", "mage", "monk",
    "paladin", "priest", "rogue", "shaman", "warlock", "warrior", "player_simplified",
    "enemy", "tank_dummy", "pet", "guardian", "copy", "armory", "local_json", "guild"
  } };

  return range::any_of( actor_scope_opts, [ &opt ]( util::string_view name ) {
    return util::str_compare_ci( opt.name, name );
  } );
}

} // unnamed

namespace profileset
{
size_t profilesets_t::done_profilesets() const
{
  if ( m_work_index <= n_workers() )
  {
    return 0;
  }

  return m_work_index - n_workers();
}

sim_control_t* profilesets_t::create_sim_options( const sim_control_t* original, const std::vector<std::string>& opts,
                                                  unsigned main_actor_index )
{
  if ( !original )
    return nullptr;

  sim_control_t new_options;

  new_options.options.parse_args( opts );

  // Find the insertion indices only once, and cache the positions to speed up init.
  if ( m_actor_indices.empty() )
  {
    for ( size_t i = 0; i < original->options.size(); i++ )
    {
      if ( is_actor_scope( original->options[ i ] ) )
        m_actor_indices.push_back( i );
    }

    if ( m_actor_indices.empty() )
    {
      throw std::invalid_argument( "No start of player-scope defined for this simulation." );
      return nullptr;
    }

    if ( main_actor_index >= m_actor_indices.size() )
    {
      throw std::invalid_argument(
        fmt::format( "'profileset_main_actor_index={}' out of range, only {} actors are defined.", main_actor_index,
                     m_actor_indices.size() ) );
      return nullptr;
    }

    // Also append an index for the past-the-end iterator,
    // which is used for consistency in implementation below.
    m_actor_indices.push_back( original->options.size() );
  }

  auto options_copy = new sim_control_t( *original );
  std::vector<option_tuple_t> filtered_opts;

  // Filter profileset options so that any option overridable in the base options is
  // overriden, and the rest are inserted at the correct position
  size_t profileset_actor_start_index = m_actor_indices[ main_actor_index ];
  size_t profileset_actor_end_index = m_actor_indices[ main_actor_index + 1 ];
  for ( const option_tuple_t& t : new_options.options )
  {
    if ( is_actor_scope( t ) )
    {
      throw std::invalid_argument(
        fmt::format( "Profilesets cannot define additional actors '{}={}'.", t.name, t.value ) );
      return nullptr;
    }

    if ( !overridable_option( t ) )
    {
      filtered_opts.push_back( t );
    }
    // Option that can be overridden in the copy, check if it exists in the base options
    // set and replace if so
    else
    {
      // Note, replace the last occurrence of the option to ensure the profileset option will be set
      auto end_it = std::reverse_iterator( options_copy->options.begin() + profileset_actor_start_index );
      auto start_it = std::reverse_iterator( options_copy->options.begin() + profileset_actor_end_index );
      auto it = std::find_if( start_it, end_it, [&t]( const option_tuple_t& orig_t ) { return orig_t.name == t.name; } );
      if ( it != end_it )
        it->value = t.value;
      else
        filtered_opts.push_back( t );
    }
  }

  options_copy -> options.insert( options_copy -> options.begin() + profileset_actor_end_index,
                                  filtered_opts.begin(), filtered_opts.end() );

  return options_copy;
}

profilesets_t::profilesets_t() : m_state( STARTED ), m_mode( SEQUENTIAL ),
  m_original( nullptr ), m_actor_indices(),
  m_work_index( 0 ),
  m_control_lock( m_mutex, std::defer_lock ),
  m_max_workers( 0 ), 
  m_work_lock( m_work_mutex, std::defer_lock ),
  m_total_elapsed(),
  m_pending_indices(),
  m_profileset_lookup(),
  m_refinement_round( 0 )
{ 

}

profilesets_t::~profilesets_t()
{
  range::for_each( m_thread, []( std::thread& thread ) {
    if ( thread.joinable() )
    {
      thread.join();
    }
  } );

  range::for_each( m_current_work, []( std::unique_ptr<worker_t>& worker ) { worker -> thread().join(); } );
}

profile_set_t::profile_set_t( std::string name, sim_control_t* opts, bool has_output ) :
  m_name( std::move(name) ), m_options( opts ), m_has_output( has_output ), m_output_data( nullptr )
{
}

sim_control_t* profile_set_t::options() const
{
  return m_options;
}

void profile_set_t::cleanup_options()
{
  delete m_options;
  m_options = nullptr;
}

profile_set_t::~profile_set_t()
{
  delete m_options;
}

const profile_result_t& profile_set_t::result( scale_metric_e metric ) const
{
  static const profile_result_t __default {};

  if ( m_results.empty() )
  {
    return __default;
  }

  if ( metric == SCALE_METRIC_NONE )
  {
    return m_results.front();
  }

  auto it = range::find_if( m_results, [ metric ]( const profile_result_t& result ) {
    return metric == result.metric();
  } );

  if ( it != m_results.end() )
  {
    return *it;
  }

  return __default;
}

profile_result_t& profile_set_t::result( scale_metric_e metric )
{
  assert( metric != SCALE_METRIC_NONE );

  auto it = range::find_if( m_results, [ metric ]( profile_result_t& result ) {
    return metric == result.metric();
  } );

  if ( it != m_results.end() )
  {
    return *it;
  }

  m_results.emplace_back( metric );

  return m_results.back();
}

worker_t::worker_t( profilesets_t* master, sim_t* p, profile_set_t* ps ) :
  m_done( false ), m_parent( p ), m_master( master ), m_sim( nullptr ), m_profileset( ps ),
  m_thread( nullptr )
{
  m_thread = new std::thread( [this] { execute(); } );
}

worker_t::~worker_t()
{
  delete m_sim;
  delete m_thread;
}

std::thread& worker_t::thread()
{
  return *m_thread;
}

const std::thread& worker_t::thread() const
{
  return *m_thread;
}

sim_t* worker_t::sim() const
{
  return m_sim;
}

void worker_t::execute()
{
  try
  {
    m_sim = new sim_t( m_parent, 0, m_profileset->options() );

    simulate_profileset( m_parent, *m_profileset, m_sim );
  }
  catch ( const std::exception& )
  {
    try
    {
      std::throw_with_nested( std::runtime_error( fmt::format( "Profileset '{}' worker", m_profileset->name() ) ) );
    }
    catch ( const std::exception& )
    {
      AUTO_LOCK( m_parent->exception_mutex );
      m_parent->exception_queue.push_back( std::current_exception() );
      // TODO: find out how to cancel profilesets without deadlock.
    }
  }

  m_done = true;

  m_master->notify_worker();
}

// Count the number of running workers
size_t profilesets_t::n_workers() const
{
  return range::count_if( m_current_work, []( const std::unique_ptr<worker_t>& worker ) {
    return ! worker -> is_done();
  } );
}

// Note, we must own the work mutex here.
void profilesets_t::cleanup_work()
{
  assert( m_work_lock.owns_lock() );

  auto it = m_current_work.begin();

  while ( it != m_current_work.end() )
  {
    if ( ( *it ) -> is_done() )
    {
      ( *it ) -> thread().join();

      auto sim = ( *it ) -> sim();

      // Pure iterative time
      m_total_elapsed += sim -> elapsed_time;

      it = m_current_work.erase( it );
    }
    else
    {
      ++it;
    }
  }
}

// Wait until we have all the work done
void profilesets_t::finalize_work()
{
  // Nothing to finalize for sequential profileset model
  if ( m_mode == SEQUENTIAL )
  {
    return;
  }

  while ( !m_current_work.empty() )
  {
    assert( ! m_work_lock.owns_lock() );

    m_work_lock.lock();

    // If we still have active workers around, wait for them to signal their finish
    // TODO: Potential race? (wait vs notify_worker() called from thread)
    if ( n_workers() > 0 )
    {
      m_work.wait( m_work_lock );
    }

    // Aand clean up finished sims
    cleanup_work();

    m_work_lock.unlock();
  }
}

void profilesets_t::generate_work( sim_t* parent, std::unique_ptr<profile_set_t>& ptr_set )
{
  if ( m_mode == SEQUENTIAL )
  {
    auto original_opts = parent->control;
    parent->control = ptr_set->options();

    try
    {
      sim_t* profile_sim = new sim_t( parent );

      parent->control = original_opts;
      simulate_profileset( parent, *ptr_set, profile_sim );

      delete profile_sim;
    }
    catch ( const std::exception& )
    {
      std::throw_with_nested( std::runtime_error( fmt::format( "Profileset '{}'", ptr_set->name() ) ) );
    }
  }
  // Parallel processing
  else
  {
    m_work_lock.lock();

    while ( ! is_done() && m_max_workers - n_workers() == 0 )
    {
      m_work.wait( m_work_lock );
    }

    if ( ! is_done() )
    {
      cleanup_work();

      // Output profileset progressbar whenever we finish anything
      output_progressbar( parent );

      m_current_work.push_back( std::make_unique<worker_t>( this, parent, ptr_set.get() ) );
    }

    m_work_lock.unlock();
  }
}

// Build sorted list of all arms by current mean
static std::vector<sim_t::profileset_cull_state_t::arm_stats_t*>
build_sorted_arms( sim_t::profileset_cull_state_t& cull )
{
  std::vector<sim_t::profileset_cull_state_t::arm_stats_t*> sorted_arms;
  sorted_arms.reserve( cull.active_arms.size() );

  for ( auto& arm : cull.active_arms )
  {
    sorted_arms.push_back( &arm );
  }

  // Sort by mean (ascending: worst to best)
  std::sort( sorted_arms.begin(), sorted_arms.end(),
    []( const sim_t::profileset_cull_state_t::arm_stats_t* a,
        const sim_t::profileset_cull_state_t::arm_stats_t* b ) {
      return a->mean < b->mean;
    } );

  return sorted_arms;
}

// Compute boundary states for all adjacent pairs in sorted order
static std::vector<sim_t::profileset_cull_state_t::boundary_state_t>
compute_boundaries(
  const std::vector<sim_t::profileset_cull_state_t::arm_stats_t*>& sorted_arms,
  sim_t::profileset_cull_state_t& cull,
  int max_iterations )
{
  std::vector<sim_t::profileset_cull_state_t::boundary_state_t> boundaries;

  if ( sorted_arms.size() < 2 )
    return boundaries;

  boundaries.reserve( sorted_arms.size() - 1 );

  for ( size_t i = 0; i + 1 < sorted_arms.size(); ++i )
  {
    auto& lower_arm = *sorted_arms[i];
    auto& upper_arm = *sorted_arms[i + 1];

    sim_t::profileset_cull_state_t::boundary_state_t b;
    b.lower_arm_idx = static_cast<int>(i);
    b.upper_arm_idx = static_cast<int>(i + 1);

    // Compute gap: U(i) - L(i+1)
    b.gap = lower_arm.upper_bound - upper_arm.lower_bound;

    // Check if separated
    b.separated = ( b.gap <= cull.indifference_zone );

    // Compute samples needed to separate
    b.m_lower_sep = cull.compute_samples_to_tighten_upper( lower_arm, upper_arm );
    b.m_upper_sep = cull.compute_samples_to_raise_lower( lower_arm, upper_arm );

    if ( cull.verbose >= 2 )
    {
      fmt::print( stderr, "  DEBUG RAW: m_lower={:.2e} (isinf={}) m_upper={:.2e} (isinf={})\n",
                  b.m_lower_sep, std::isinf(b.m_lower_sep),
                  b.m_upper_sep, std::isinf(b.m_upper_sep) );
    }

    // Check futility: both sides infeasible
    int remaining_lower = max_iterations - lower_arm.iterations;
    int remaining_upper = max_iterations - upper_arm.iterations;

    // Special case: both one-sided operations impossible (both ∞ or effectively ∞)
    // This means we need to sample BOTH arms together - do NOT defer
    // Use practical infinity threshold because very small gaps can produce huge finite values
    const double practical_infinity = 1e15;
    const bool both_infinite =
      ( !std::isfinite( b.m_lower_sep ) || b.m_lower_sep > practical_infinity ) &&
      ( !std::isfinite( b.m_upper_sep ) || b.m_upper_sep > practical_infinity );

    if ( both_infinite )
    {
      b.deferred = false;  // Let scheduler's "queue both" logic handle this
      if ( cull.verbose >= 2 )
      {
        fmt::print( stderr, "  DEBUG: Both infinite (>1e15) - NOT deferring\n" );
      }
    }
    // Hard-defer if both sides exceed total budget
    else if ( b.m_lower_sep > remaining_lower && b.m_upper_sep > remaining_upper )
    {
      b.deferred = true;
      if ( cull.verbose >= 2 )
      {
        fmt::print( stderr, "  DEBUG: Hard defer (both exceed budget)\n" );
      }
    }
    else
    {
      b.deferred = false;
      if ( cull.verbose >= 2 )
      {
        fmt::print( stderr, "  DEBUG: Not deferred (at least one feasible)\n" );
      }
    }

    // Verbose diagnostics for boundaries
    if ( cull.verbose >= 2 )
    {
      auto format_samples = []( double m ) -> std::string {
        if ( std::isinf( m ) ) return "∞";
        if ( m > 1e15 ) return "∞";
        if ( m > 1e6 ) return fmt::format( "{:.2e}", m );
        return fmt::format( "{:.0f}", m );
      };

      fmt::print( stderr, "profileset_cull: boundary [{} vs {}]:\n",
                  lower_arm.name, upper_arm.name );
      fmt::print( stderr, "  Lower: mean={:.1f} var={:.1f} U={:.1f} n={}\n",
                  lower_arm.mean, lower_arm.variance, lower_arm.upper_bound, lower_arm.iterations );
      fmt::print( stderr, "  Upper: mean={:.1f} var={:.1f} L={:.1f} n={}\n",
                  upper_arm.mean, upper_arm.variance, upper_arm.lower_bound, upper_arm.iterations );
      fmt::print( stderr, "  Gap: U(lower) - L(upper) = {:.1f}\n", b.gap );
      fmt::print( stderr, "  Samples to separate: tighten_lower={} raise_upper={} deferred={} separated={}\n",
                  format_samples( b.m_lower_sep ), format_samples( b.m_upper_sep ),
                  b.deferred ? "true" : "false", b.separated ? "true" : "false" );
    }

    boundaries.push_back( b );
  }

  return boundaries;
}

// Enhanced multi-boundary selection using priority scoring
// Returns vector of boundary indices to target, ranked by priority
// Can return multiple boundaries if they don't share arms and have similar priorities
static std::vector<int> select_refinement_targets(
  const std::vector<sim_t::profileset_cull_state_t::boundary_state_t>& boundaries,
  const std::vector<sim_t::profileset_cull_state_t::arm_stats_t*>& sorted_arms,
  sim_t::profileset_cull_state_t& cull,
  int max_iterations )
{
  std::vector<int> selected;

  if ( cull.verbose >= 3 )
  {
    fmt::print( stderr, "profileset_cull: Selecting refinement targets from {} boundaries\n", boundaries.size() );
  }

  if ( boundaries.empty() )
    return selected;

  // Compute priority for each boundary
  struct boundary_with_priority {
    int index;
    double priority;
    double m_lower;
    double m_upper;
    int lower_arm_idx;
    int upper_arm_idx;
  };

  std::vector<boundary_with_priority> ranked;
  ranked.reserve( boundaries.size() );

  for ( size_t i = 0; i < boundaries.size(); ++i )
  {
    const auto& b = boundaries[i];

    if ( cull.verbose >= 3 )
    {
      fmt::print( stderr, "  DEBUG: Evaluating boundary #{} ({} vs {})\n",
                  i, sorted_arms[b.lower_arm_idx]->name, sorted_arms[b.upper_arm_idx]->name );
    }

    // Skip separated or deferred boundaries
    if ( b.separated || b.deferred ) {
      if ( cull.verbose >= 3 )
      {
        fmt::print( stderr, "    Skipping (separated={} deferred={})\n",
                    b.separated ? "true" : "false",
                    b.deferred ? "true" : "false" );
      }
      continue;
    }

    auto* lower_arm = sorted_arms[b.lower_arm_idx];
    auto* upper_arm = sorted_arms[b.upper_arm_idx];

    int remaining_lower = max_iterations - lower_arm->iterations;
    int remaining_upper = max_iterations - upper_arm->iterations;

    double priority = cull.compute_boundary_priority(
        *lower_arm, *upper_arm,
        b.gap, b.m_lower_sep, b.m_upper_sep,
        remaining_lower, remaining_upper,
        max_iterations );

    if ( cull.verbose >= 3 )
    {
      auto format_samples = []( double m ) -> std::string {
      if ( std::isinf( m ) ) return "∞";
      if ( m > 1e15 ) return "∞";
      if ( m > 1e6 ) return fmt::format( "{:.2e}", m );
      return fmt::format( "{:.0f}", m );
      };

      bool critical = cull.is_boundary_critical( *lower_arm, *upper_arm, cull.active_arms );
      fmt::print( stderr,
            "    Priority result: priority={:.4g} gap={:.2f} m_lower={} m_upper={} "
            "remain_lower={} remain_upper={} lower_mean={:.2f} upper_mean={:.2f} "
            "lower_n={} upper_n={} critical={}\n",
            priority, b.gap,
            format_samples( b.m_lower_sep ), format_samples( b.m_upper_sep ),
            remaining_lower, remaining_upper,
            lower_arm->mean, upper_arm->mean,
            lower_arm->iterations, upper_arm->iterations,
            critical ? "true" : "false" );
    }

    // Skip if zero priority (infeasible)
    if ( priority <= 0.0 )
      continue;

    // Boost priority if boundary is critical
    if ( cull.is_boundary_critical( *lower_arm, *upper_arm, cull.active_arms ) )
      priority *= 1.5;

    ranked.push_back( { static_cast<int>(i), priority, b.m_lower_sep, b.m_upper_sep,
                        b.lower_arm_idx, b.upper_arm_idx } );
  }

  if ( ranked.empty() )
    return selected;

  // Sort by priority (descending)
  std::sort( ranked.begin(), ranked.end(),
             []( const boundary_with_priority& a, const boundary_with_priority& b ) {
               return a.priority > b.priority;
             } );

  // Select top boundary
  selected.push_back( ranked[0].index );

  // Track which arms are already targeted
  std::set<int> targeted_arm_indices;
  targeted_arm_indices.insert( ranked[0].lower_arm_idx );
  targeted_arm_indices.insert( ranked[0].upper_arm_idx );

  // Consider adding more boundaries if they don't conflict and have decent priority
  const double priority_threshold = ranked[0].priority * 0.6;  // Within 60% of best priority

  for ( size_t i = 1; i < ranked.size() && selected.size() < 3; ++i )
  {
    const auto& candidate = ranked[i];

    // Skip if priority too low
    if ( candidate.priority < priority_threshold )
      break;

    // Skip if shares arms with already-selected boundaries
    if ( targeted_arm_indices.count( candidate.lower_arm_idx ) > 0 ||
         targeted_arm_indices.count( candidate.upper_arm_idx ) > 0 )
      continue;

    // Add this boundary
    selected.push_back( candidate.index );
    targeted_arm_indices.insert( candidate.lower_arm_idx );
    targeted_arm_indices.insert( candidate.upper_arm_idx );
  }

  return selected;
}

bool profilesets_t::schedule_refinement_pass( sim_t* parent )
{
  if ( !parent || !parent->profileset_cull.enabled )
    return false;

  auto& cull = parent->profileset_cull;
  if ( cull.method != sim_t::profileset_cull_state_t::CONFSEQ_TOTAL_ORDER )
    return false;

  std::vector<size_t> next_indices;
  std::string lower_arm_name, upper_arm_name;
  double boundary_gap = 0.0;
  double m_lower = 0.0, m_upper = 0.0;
  size_t num_selected_boundaries = 0;  // Track for logging

  {
    std::lock_guard<mutex_t> lock( cull.mtx );
    if ( !cull.bootstrap_complete )
      return false;

    // 1. Build sorted arms list
    auto sorted_arms = build_sorted_arms( cull );

    // 2. Compute all boundaries
    auto boundaries = compute_boundaries( sorted_arms, cull, parent->iterations );

    // 2a. Print summary progress (every 10 passes or if verbose)
    static int last_summary_pass = 0;
    if ( cull.verbose >= 1 || ( cull.refinement_pass_count > 0 && cull.refinement_pass_count % 10 == 0 && cull.refinement_pass_count != last_summary_pass ) )
    {
      int separated = 0, deferred = 0, active = 0;
      double min_gap = std::numeric_limits<double>::infinity();
      double max_gap = 0.0;
      int both_infinite = 0, one_infinite = 0, both_finite = 0;

      for ( const auto& b : boundaries )
      {
        if ( b.separated ) separated++;
        else if ( b.deferred ) deferred++;
        else {
          active++;
          min_gap = std::min( min_gap, b.gap );
          max_gap = std::max( max_gap, b.gap );

          const double practical_infinity = 1e15;
          bool lower_inf = !std::isfinite(b.m_lower_sep) || b.m_lower_sep > practical_infinity;
          bool upper_inf = !std::isfinite(b.m_upper_sep) || b.m_upper_sep > practical_infinity;

          if ( lower_inf && upper_inf ) both_infinite++;
          else if ( lower_inf || upper_inf ) one_infinite++;
          else both_finite++;
        }
      }

      fmt::print( stderr, "\nprofileset_cull: PROGRESS SUMMARY (pass #{})\n", cull.refinement_pass_count );
      fmt::print( stderr, "  Boundaries: {} separated, {} active, {} deferred (total {})\n",
                  separated, active, deferred, boundaries.size() );
      if ( active > 0 )
      {
        fmt::print( stderr, "  Active gap range: [{:.0f}, {:.0f}]\n", min_gap, max_gap );
        fmt::print( stderr, "  Sample requirements: {} both-∞, {} one-∞, {} both-finite\n",
                    both_infinite, one_infinite, both_finite );
      }
      fmt::print( stderr, "\n" );

      last_summary_pass = cull.refinement_pass_count;
    }

    // 3. Select refinement targets using enhanced priority-based selection
    auto selected_boundaries = select_refinement_targets( boundaries, sorted_arms, cull, parent->iterations );

    if ( selected_boundaries.empty() )
    {
      // All boundaries separated or deferred - print final summary
      int separated = 0, deferred = 0;
      for ( const auto& b : boundaries )
      {
        if ( b.separated ) separated++;
        else if ( b.deferred ) deferred++;
      }

      fmt::print( stderr, "\nprofileset_cull: REFINEMENT COMPLETE after {} passes\n", cull.refinement_pass_count );
      fmt::print( stderr, "  Final: {} boundaries separated, {} deferred\n", separated, deferred );

      return false;
    }

    // 4. Process all selected boundaries
    // Store info from primary (first) boundary for logging
    auto& primary_b = boundaries[selected_boundaries[0]];
    auto* primary_lower_arm = sorted_arms[primary_b.lower_arm_idx];
    auto* primary_upper_arm = sorted_arms[primary_b.upper_arm_idx];

    lower_arm_name = primary_lower_arm->name;
    upper_arm_name = primary_upper_arm->name;
    boundary_gap = primary_b.gap;
    m_lower = primary_b.m_lower_sep;
    m_upper = primary_b.m_upper_sep;
    num_selected_boundaries = selected_boundaries.size();

    // Process all selected boundaries
    const double cost_ratio = 1.2;  // 20% tolerance for "similar cost"
    const double practical_infinity = 1e15;  // Match deferral logic threshold

    // Track arms we've already queued to avoid duplicates
    std::set<std::string> queued_arms;

    for ( int boundary_idx : selected_boundaries )
    {
      auto& b = boundaries[boundary_idx];
      auto* lower_arm = sorted_arms[b.lower_arm_idx];
      auto* upper_arm = sorted_arms[b.upper_arm_idx];

      bool queue_lower = false;
      bool queue_upper = false;

      // Check for effectively infinite values (may be huge finite numbers instead of true infinity)
      const bool lower_infinite = !std::isfinite(b.m_lower_sep) || b.m_lower_sep > practical_infinity;
      const bool upper_infinite = !std::isfinite(b.m_upper_sep) || b.m_upper_sep > practical_infinity;

      if ( lower_infinite && upper_infinite )
      {
        // Both infinite - need to sample both arms together
        queue_lower = queue_upper = true;
      }
      else if ( lower_infinite )
      {
        // Only upper is feasible
        queue_upper = true;
      }
      else if ( upper_infinite )
      {
        // Only lower is feasible
        queue_lower = true;
      }
      else if ( b.m_lower_sep < b.m_upper_sep * cost_ratio )
      {
        // Lower is cheaper (or within 20% tolerance)
        queue_lower = true;
        // Also queue upper if costs are very similar (both within tolerance)
        if ( b.m_upper_sep < b.m_lower_sep * cost_ratio )
          queue_upper = true;
      }
      else
      {
        // Upper is cheaper
        queue_upper = true;
      }

    // Queue selected arms (macro-batch planning)
    const double kappa = 2.0;       // per-arm macro cap: m_a <= kappa * n_a
    const double gamma_min = 1.75;  // minimum geometric jump when both sides must move

    auto plan_for_side = [&](const sim_t::profileset_cull_state_t::arm_stats_t& arm,
                              double m_star, bool both_infinite, double w_sum, double gap) -> int {
      int remaining = parent->iterations - arm.iterations;
      if ( remaining <= 0 ) return 0;
      double planned = 0.0;
      if ( !both_infinite && std::isfinite(m_star) )
      {
        planned = std::ceil( m_star );                     // one-sided finite: take the shot
      }
      else
      {
        // both-infinite (or this side infinite): geometric jump m = (gamma-1) * n
        double ratio = (gap > 0.0) ? (w_sum / gap) : gamma_min;
        double gamma = std::max( gamma_min, ratio * ratio );
        planned = std::ceil( (gamma - 1.0) * static_cast<double>( arm.iterations ) );
      }
      // Caps & floors
      planned = std::max( planned, static_cast<double>( cull.min_iterations ) );            // avoid micro-batch
      planned = std::min( planned, static_cast<double>( remaining ) );                      // within arm budget
      planned = std::min( planned, kappa * static_cast<double>( arm.iterations ) );         // avoid overshoot
      if ( planned < 0.0 || !std::isfinite(planned) ) return 0;
      return static_cast<int>( planned );
    };

    // Precompute widths and geometric parameters for both-infinite case
    const bool both_inf = ( !std::isfinite(m_lower) || m_lower > 1e15 ) &&
                          ( !std::isfinite(m_upper) || m_upper > 1e15 );
    const double w_lower = std::max( 0.0, lower_arm->upper_bound - lower_arm->mean );
    const double w_upper = std::max( 0.0, upper_arm->mean - upper_arm->lower_bound );
    const double w_sum   = w_lower + w_upper;
    const double gap_pos = std::max( 1e-12, boundary_gap ); // b.gap = U_l - L_u; overlap => positive

      if ( queue_lower && queued_arms.find( lower_arm->name ) == queued_arms.end() )
      {
        auto it = m_profileset_lookup.find( lower_arm->name );
        if ( it != m_profileset_lookup.end() && lower_arm->iterations < parent->iterations )
        {
          lower_arm->refinement_start_iterations = lower_arm->iterations;
          const int planned = plan_for_side( *lower_arm, m_lower, both_inf, w_sum, gap_pos );
          lower_arm->refinement_start_iterations  = lower_arm->iterations;
          lower_arm->refinement_target_iterations = lower_arm->iterations + planned;
          next_indices.push_back( it->second );
          queued_arms.insert( lower_arm->name );
        }
      }

      if ( queue_upper && queued_arms.find( upper_arm->name ) == queued_arms.end() )
      {
        auto it = m_profileset_lookup.find( upper_arm->name );
        if ( it != m_profileset_lookup.end() && upper_arm->iterations < parent->iterations )
        {
          upper_arm->refinement_start_iterations = upper_arm->iterations;
          const int planned = plan_for_side( *upper_arm, m_upper, both_inf, w_sum, gap_pos );
          upper_arm->refinement_start_iterations  = upper_arm->iterations;
          upper_arm->refinement_target_iterations = upper_arm->iterations + planned;
          next_indices.push_back( it->second );
          queued_arms.insert( upper_arm->name );
        }
      }
    }
  }

  if ( next_indices.empty() )
    return false;

  {
    std::lock_guard<std::mutex> guard( m_mutex );
    m_pending_indices = std::move( next_indices );
    m_work_index = 0;
    ++m_refinement_round;
  }

  // Increment pass counter
  cull.refinement_pass_count++;

  // Telemetry logging
  if ( cull.verbose >= 1 )
  {
    if ( num_selected_boundaries == 1 )
    {
      fmt::print( stderr, "\nprofileset_cull: refinement pass #{} targeting boundary ('{}' vs '{}')\n",
                  cull.refinement_pass_count, lower_arm_name, upper_arm_name );
    }
    else
    {
      fmt::print( stderr, "\nprofileset_cull: refinement pass #{} targeting {} boundaries (primary: '{}' vs '{}')\n",
                  cull.refinement_pass_count, num_selected_boundaries, lower_arm_name, upper_arm_name );
    }

    if ( cull.verbose >= 2 )
    {
      auto format_samples = []( double m ) -> std::string {
        if ( std::isinf( m ) ) return "∞";
        if ( m > 1e15 ) return "∞";
        return fmt::format( "{:.0f}", m );
      };

      fmt::print( stderr, "  primary gap={:.2f}, m_lower={}, m_upper={}, queuing {} arm(s) total\n",
                  boundary_gap, format_samples(m_lower), format_samples(m_upper),
                  m_pending_indices.size() );
    }
  }

  return true;
}

// Ensure profileset options are valid, and also perform basic simulator initialization for the
// profileset to ensure that we can launch it when the time comes
bool profilesets_t::parse( sim_t* sim )
{
  while ( true )
  {
    if ( sim->canceled )
    {
      set_state( DONE );
      m_control.notify_one();
      return false;
    }

    m_mutex.lock();

    if ( m_init_index == sim->profileset_map.cend() )
    {
      m_mutex.unlock();
      break;
    }

    const auto& profileset_name = m_init_index->first;
    const auto& profileset_opts = m_init_index->second;

    ++m_init_index;

    m_mutex.unlock();

    sim_control_t* control = nullptr;
    bool has_output_opts = false;

    try
    {
      try
      {
        control = create_sim_options( m_original.get(), profileset_opts, sim->profileset_main_actor_index );
        if ( !control )
        {
          set_state( DONE );
          m_control.notify_one();
          return false;
        }
      }
      catch ( const std::exception& )
      {
        set_state( DONE );
        m_control.notify_one();
        if ( control )
          delete control;

        std::throw_with_nested( sc_invalid_sim_argument( "Invalid profileset option" ) );
      }

      has_output_opts = range::any_of( profileset_opts, []( std::string_view opt ) {
        auto name_end = opt.find( "=" );
        if ( name_end == std::string::npos )
          return false;

        auto name = opt.substr( 0, name_end );

        return util::str_compare_ci( name, "output" ) || util::str_compare_ci( name, "html" ) ||
               util::str_compare_ci( name, "json2" );
      } );

      // Test that profileset options are OK, up to the simulation initialization
      auto test_sim = std::make_unique<sim_t>();
      test_sim->profileset_enabled = true;
      test_sim->setup( control );
      test_sim->init();
    }
    catch ( const std::exception& )
    {
      try
      {
        set_state( DONE );
        m_control.notify_one();
        if ( control )
          delete control;

        std::throw_with_nested( std::runtime_error( fmt::format( "Profileset '{}'", profileset_name ) ) );
      }
      catch ( const std::exception& )
      {
        AUTO_LOCK( sim->exception_mutex );
        sim->exception_queue.push_back( std::current_exception() );
        return false;
      }
    }

  m_mutex.lock();
  m_profilesets.push_back( std::make_unique<profile_set_t>( profileset_name, control, has_output_opts ) );
  m_profileset_lookup[ profileset_name ] = m_profilesets.size() - 1;
    m_control.notify_one();
    m_mutex.unlock();
  }

  set_state( RUNNING );

  return true;
}

void profilesets_t::initialize( sim_t* sim )
{
  if ( sim->profileset_enabled || sim->parent || sim->thread_index > 0 )
  {
    return;
  }

  if ( sim->profileset_map.empty() )
  {
    set_state( DONE );
    return;
  }

  if ( sim->profileset_report_player_index >= sim->player_no_pet_list.size() )
  {
    throw sc_invalid_sim_argument( fmt::format( "'profileset_report_player_index={}' out of range, only {} players.",
                                                sim->profileset_report_player_index, sim->player_no_pet_list.size() ) );
  }

  if ( sim->profileset_init_threads < 1 )
  {
    sim->error( "No profileset init threads given, profilesets cannot continue" );
    return;
  }

  // Figure out how many workers can we have by looking at how many threads we have, and how many
  // worker threads the user wants
  if ( sim->profileset_work_threads > 0 )
  {
    size_t workers = as<size_t>( sim->threads / sim->profileset_work_threads );
    if ( workers == 0 )
    {
      sim->error( "More worker threads defined than simulator threads, reverting to sequential behavior" );
      sim->profileset_work_threads = 0;
    }

    m_max_workers = workers;
  }

  // Go parallel mode if we have any workers .. including one
  if ( m_max_workers > 0 )
  {
    m_mode = PARALLEL;
  }

  m_profilesets.reserve( sim -> profileset_map.size() + 1 );

  // Generate a copy of the original control, and remove any and all profileset. options from it
  m_original = std::make_unique<sim_control_t>( );

  // Copy non-profileset. options to use as a base option setup for each profileset
  range::copy_if( sim -> control -> options, std::back_inserter( m_original -> options ),
    []( const option_tuple_t& opt ) {
    return ! util::str_in_str_ci( opt.name, "profileset." );
  } );

  // Spawn initialization threads, and start parsing through the profilesets
  set_state( INITIALIZING );

  m_init_index = sim -> profileset_map.cbegin();

  for ( int i = 0; i < sim -> profileset_init_threads; ++i )
  {
    m_thread.emplace_back([ this, sim ]() {
      if ( ! parse( sim ) )
      {
        sim -> cancel();
      }
    } );
  }
}

void profilesets_t::cancel()
{
  if ( ! is_done() )
  {
    range::for_each( m_thread, []( std::thread& thread ) {
      if ( thread.joinable() )
      {
        thread.join();
      }
    } );
  }

  set_state( DONE );
}

void profilesets_t::set_state( state new_state )
{
  m_mutex.lock();

  m_state = new_state;

  m_mutex.unlock();
}

std::string profilesets_t::current_profileset_name()
{
  m_control_lock.lock();
  if ( is_done() || m_work_index == 0 )
  {
    m_control_lock.unlock();
    return {};
  }

  size_t queue_pos = m_work_index - 1;
  size_t idx = queue_pos;
  if ( !m_pending_indices.empty() && queue_pos < m_pending_indices.size() )
  {
    idx = m_pending_indices[ queue_pos ];
  }

  std::string profileset_name = m_profilesets[ idx ] -> name();
  m_control_lock.unlock();

  return profileset_name;
}

bool profilesets_t::iterate( sim_t* parent )
{
  if ( parent -> profileset_map.empty() )
  {
    return true;
  }

  auto original_opts = parent -> control;

  m_start_time = chrono::wall_clock::now();

  while ( ! is_done() )
  {
    m_control_lock.lock();

    // Wait until we have at least something to sim
    while ( is_initializing() && m_profilesets.size() - m_work_index == 0 )
    {
      m_control.wait( m_control_lock );
    }

    if ( is_running() )
    {
      if ( m_pending_indices.empty() )
      {
        m_pending_indices.resize( m_profilesets.size() );
        std::iota( m_pending_indices.begin(), m_pending_indices.end(), 0 );
        m_work_index = 0;
        m_refinement_round = 0;
      }

      if ( m_work_index >= m_pending_indices.size() )
      {
        m_control_lock.unlock();
        if ( schedule_refinement_pass( parent ) )
        {
          continue;
        }
        break;
      }
    }

    size_t queue_pos = m_work_index++;
    size_t idx = queue_pos;
    if ( !m_pending_indices.empty() && queue_pos < m_pending_indices.size() )
    {
      idx = m_pending_indices[ queue_pos ];
    }

    auto& set = m_profilesets[ idx ];

    m_control_lock.unlock();

    generate_work( parent, set );
  }

  if ( parent->profileset_cull.enabled &&
       parent->profileset_cull.method == sim_t::profileset_cull_state_t::CONFSEQ_TOTAL_ORDER )
  {
    m_work_index = m_profilesets.size();
  }

  // Wait until the tail-end of the parallel work has been done. Non-parallel processing mode will
  // not need to finalize any work (all work has been done by the loop above)
  finalize_work();

  // Output profileset progressbar whenever we finish anything
  output_progressbar( parent );

  // Update parent elapsed_time
  parent -> elapsed_time += chrono::elapsed( m_start_time );

  parent -> control = original_opts;

  for ( auto& set : m_profilesets )
  {
    if ( set && set->options() )
    {
      set->cleanup_options();
    }
  }

  set_state( DONE );

  // Print total ordering summary if using CONFSEQ_TOTAL_ORDER
  if ( parent->profileset_cull.enabled &&
       parent->profileset_cull.method == sim_t::profileset_cull_state_t::CONFSEQ_TOTAL_ORDER &&
       parent->profileset_cull.verbose >= 1 )
  {
    // Build sorted list of all arms
    std::vector<const sim_t::profileset_cull_state_t::arm_stats_t*> sorted_arms;
    for ( const auto& arm : parent->profileset_cull.active_arms )
    {
      sorted_arms.push_back( &arm );
    }

    if ( !sorted_arms.empty() )
    {
      std::sort( sorted_arms.begin(), sorted_arms.end(),
                []( const sim_t::profileset_cull_state_t::arm_stats_t* a,
                    const sim_t::profileset_cull_state_t::arm_stats_t* b ) {
                  return a->mean < b->mean;
                } );

      fmt::print( stderr, "\n=== Total Ordering Summary (CONFSEQ) ===\n" );
      fmt::print( stderr, "Rank  Name                           Mean       CI [Lower, Upper]        Status\n" );
      fmt::print( stderr, "----  ----------------------------  ---------  ---------------------    ------\n" );

      for ( size_t i = 0; i < sorted_arms.size(); ++i )
      {
        const auto& arm = *sorted_arms[i];
        const char* status = arm.active ? "Active" : "Futile";

        // Check if boundary with next arm is separated
        bool separated_next = true;
        if ( i + 1 < sorted_arms.size() )
        {
          const auto& next_arm = *sorted_arms[i + 1];
          const double gap = arm.upper_bound - next_arm.lower_bound;
          separated_next = ( gap <= parent->profileset_cull.indifference_zone );
        }

        const char* separator = separated_next ? "  " : "~~";

        fmt::print( stderr, "{:4}  {:30}  {:9.2f}  [{:9.2f}, {:9.2f}]  {:6}\n",
                    i + 1, arm.name, arm.mean, arm.lower_bound, arm.upper_bound, status );

        if ( !separated_next && i + 1 < sorted_arms.size() )
        {
          const auto& lower_arm = *sorted_arms[i];
          const auto& upper_arm = *sorted_arms[i + 1];

          // Compute samples needed to separate by tightening lower arm's upper bound
          const double m_lower = parent->profileset_cull.compute_samples_to_tighten_upper( lower_arm, upper_arm );

          // Compute samples needed to separate by raising upper arm's lower bound
          const double m_upper = parent->profileset_cull.compute_samples_to_raise_lower( lower_arm, upper_arm );

          // Compute remaining budget for each arm
          const int remaining_lower = parent->iterations - lower_arm.iterations;
          const int remaining_upper = parent->iterations - upper_arm.iterations;

          // Format helper for large/infinite values
          auto format_samples = []( double m ) -> std::string {
            if ( std::isinf( m ) ) return "∞";
            if ( m > 1e15 ) return "∞";
            return fmt::format( "{:.0f}", m );
          };

          fmt::print( stderr, "      {}  (overlapping: need {} vs {} samples, remaining {} vs {})\n",
                      separator,
                      format_samples(m_lower), format_samples(m_upper),
                      remaining_lower, remaining_upper );
        }
      }

      fmt::print( stderr, "=========================================\n\n" );
    }
  }

  // rethrow any accumulated exceptions
  if ( parent->rethrow_exception_queue() )
    return false;

  return true;
}

void profilesets_t::notify_worker()
{
  m_work.notify_one();
}

int profilesets_t::max_name_length() const
{
  size_t len = 0;

  range::for_each( m_profilesets, [ &len ]( const profileset_entry_t& profileset ) {
    if ( profileset -> name().size() > len )
    {
      len = profileset -> name().size();
    }
  } );

  return as<int>(len);
}

void profilesets_t::output_progressbar( const sim_t* parent ) const
{
  if ( m_max_workers == 0 )
  {
    return;
  }

  std::stringstream s;

  s << "Profilesets (" << m_max_workers << "*" << parent -> profileset_work_threads << "): ";

  auto done = done_profilesets();
  auto pct = done / as<double>( m_profilesets.size() );

  s << done << "/" << m_profilesets.size() << " ";

  std::string status = "[";
  status.insert( 1, parent -> progress_bar.steps, '.' );
  status += "]";

  int length = as<int>( std::lround( parent -> progress_bar.steps * pct ) );
  for ( int i = 1; i < length + 1; ++i )
  {
    status[ i ] = '=';
  }

  if ( length > 0 )
  {
    status[ length ] = '>';
  }

  s << status;

  auto average_per_sim = chrono::to_fp_seconds(m_total_elapsed) / as<double>( done );
  auto elapsed = chrono::elapsed_fp_seconds( m_start_time );
  auto work_left = m_profilesets.size() - done;
  auto time_left = work_left * ( average_per_sim / m_max_workers );

  // Average time per done simulation
  s << " avg=" << format_time( average_per_sim / as<double>( m_max_workers ) );

  // Elapsed time
  s << " done=" << format_time( elapsed, false );

  // Estimated time left, based on average time per done simulation, elapsed time, and the number of
  // workers
  s << " left=" << format_time( time_left, false );

  // Cleanups
  s << "     ";

  s << '\r';

  std::cout << s.str();
  std::fflush( stdout );
}

std::vector<const profile_set_t*> profilesets_t::generate_sorted_profilesets( bool mean ) const
{
  std::vector<const profile_set_t*> out;
  range::transform( m_profilesets, std::back_inserter( out ), []( const profileset_entry_t& p ) {
    return p.get();
  } );

  // Sort to descending with mean value
  range::sort( out, [mean]( const profile_set_t* l, const profile_set_t* r ) {
    double lv = mean ? l->result().mean() : l->result().median();
    double rv = mean ? r->result().mean() : r->result().median();
    if ( lv == rv )
    {
      return l->name() < r->name();
    }

    return lv > rv;
  } );
  return out;
}

size_t profilesets_t::n_profilesets() const
{
  return m_profilesets.size();
}

const profilesets_t::profileset_vector_t& profilesets_t::profilesets() const
{
  return m_profilesets;
}

bool profilesets_t::is_initializing() const
{
  return m_state == INITIALIZING;
}

bool profilesets_t::is_running() const
{
  return m_state == RUNNING;
}

bool profilesets_t::is_done() const
{
  return m_state == DONE;
}

bool profilesets_t::is_sequential() const
{
  return m_mode == SEQUENTIAL;
}

bool profilesets_t::is_parallel() const
{
  return m_mode == PARALLEL;
}

void create_options( sim_t* sim )
{
  sim -> add_option( opt_map_list( "profileset.", sim -> profileset_map ) );
  sim -> add_option( opt_uint( "profileset_main_actor_index", sim -> profileset_main_actor_index ) );
  sim -> add_option( opt_uint( "profileset_report_player_index", sim -> profileset_report_player_index ) );
  sim -> add_option( opt_string( "profileset_multiactor_base_name", sim -> profileset_multiactor_base_name ) );
  sim -> add_option( opt_func( "profileset_metric", []( sim_t*             sim,
                                                        util::string_view,
                                                        util::string_view value ) {
    sim -> profileset_metric.clear();

    auto split = util::string_split<util::string_view>( value, "/:," );
    for ( const auto& v : split )
    {
      auto metric = util::parse_scale_metric( v );
      if ( metric == SCALE_METRIC_NONE )
      {
        sim -> error( "Invalid profileset metric '{}'", v );
        return false;
      }

      sim -> profileset_metric.push_back( metric );
    }

    return true;
  } ) );
  sim -> add_option( opt_func( "profileset_output_data", []( sim_t*             sim,
                                                        util::string_view,
                                                        util::string_view value ) {
    sim -> profileset_output_data.clear();

    for ( auto&& v : util::string_split( value, "/:," ) )
    {
      sim -> profileset_output_data.push_back( std::move( v ) );
    }

    return true;
  } ) );

  sim -> add_option( opt_int( "profileset_work_threads", sim -> profileset_work_threads ) );
  sim -> add_option( opt_int( "profileset_init_threads", sim -> profileset_init_threads ) );
}

statistical_data_t collect( const extended_sample_data_t& c )
{
  return { c.min(), c.percentile( 0.25 ), c.percentile( 0.5 ), c.mean(),
           c.percentile( 0.75 ), c.max(), c.std_dev, c.mean_std_dev };
}

statistical_data_t metric_data( const player_t* player, scale_metric_e metric )
{
  const auto& d = player -> collected_data;

  switch ( metric )
  {
    case SCALE_METRIC_DPS:       return collect( d.dps );
    case SCALE_METRIC_DPSE:      return collect( d.dpse );
    case SCALE_METRIC_HPS:       return collect( d.hps );
    case SCALE_METRIC_HPSE:      return collect( d.hpse );
    case SCALE_METRIC_APS:       return collect( d.aps );
    case SCALE_METRIC_DPSP:      return collect( d.prioritydps );
    case SCALE_METRIC_DTPS:      return collect( d.dtps );
    case SCALE_METRIC_DMG_TAKEN: return collect( d.dmg_taken );
    case SCALE_METRIC_HTPS:      return collect( d.htps );
    case SCALE_METRIC_DEATHS:    return collect( d.deaths );
    case SCALE_METRIC_TIME:      return collect( d.fight_length );
    case SCALE_METRIC_RAID_DPS:  return collect( player->sim->raid_dps );
    case SCALE_METRIC_HAPS:
    {
      auto hps = collect( d.hps );
      auto aps = collect( d.aps );
      return {
        hps.min + aps.min,
        hps.first_quartile + aps.first_quartile,
        hps.median + aps.median,
        hps.mean + aps.mean,
        hps.third_quartile + aps.first_quartile,
        hps.max + aps.max,
        sqrt( d.aps.variance + d.hps.variance ),
        sqrt( d.aps.mean_variance + d.hps.mean_variance )
      };
    }
    default:                     return { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };
  }
}

void save_output_data( profile_set_t& profileset, const player_t* parent_player, const player_t* player,
                       const std::string& option )
{
  // TODO: Make an enum to proper use a switch instead of if/else
  if ( option == "race" )
  {
    if ( parent_player->race != player->race )
    {
      profileset.output_data().race( player->race );
    }
  }
  else if ( option == "gear" )
  {
    const auto& parent_items = parent_player->items;
    const auto& items = player->items;
    std::vector<profile_output_data_item_t> saved_gear;
    for ( slot_e slot = SLOT_MIN; slot < SLOT_MAX; slot++ )
    {
      const auto& item = items[ slot ];
      if ( !item.active() || !item.has_stats() )
      {
        continue;
      }
      const auto& parent_item = parent_items[ slot ];
      if ( parent_item.parsed.data.id != item.parsed.data.id )
      {
        profile_output_data_item_t saved_item{ item.slot_name(), item.parsed.data.id, item.item_level() };

        // saved_item.bonus_id( item.parsed.bonus_id );

        saved_gear.push_back( saved_item );
      }
    }
    if ( !saved_gear.empty() )
    {
      profileset.output_data().gear( saved_gear );
    }
  }
  else if ( option == "stats" )
  {
    const auto& buffed_stats = player->collected_data.buffed_stats_snapshot;

    // primary stats

    profileset.output_data().stamina( util::floor( buffed_stats.attribute[ ATTR_STAMINA ] ) );
    profileset.output_data().agility( util::floor( buffed_stats.attribute[ ATTR_AGILITY ] ) );
    profileset.output_data().intellect( util::floor( buffed_stats.attribute[ ATTR_INTELLECT ] ) );
    profileset.output_data().strength( util::floor( buffed_stats.attribute[ ATTR_STRENGTH ] ) );

    // secondary stats

    profileset.output_data().crit_rating(
      util::floor( player->composite_melee_crit_rating() > player->composite_spell_crit_rating()
                     ? player->composite_melee_crit_rating()
                     : player->composite_spell_crit_rating() ) );
    profileset.output_data().crit_pct( buffed_stats.attack_crit_chance > buffed_stats.spell_crit_chance
                                         ? buffed_stats.attack_crit_chance
                                         : buffed_stats.spell_crit_chance );

    profileset.output_data().haste_rating(
      util::floor( player->composite_melee_haste_rating() > player->composite_spell_haste_rating()
                     ? player->composite_melee_haste_rating()
                     : player->composite_spell_haste_rating() ) );

    double attack_haste_pct = 1 / buffed_stats.attack_haste - 1;
    double spell_haste_pct = 1 / buffed_stats.spell_haste - 1;
    profileset.output_data().haste_pct( attack_haste_pct > spell_haste_pct ? attack_haste_pct : spell_haste_pct );

    profileset.output_data().mastery_rating( util::floor( player->composite_mastery_rating() ) );
    profileset.output_data().mastery_pct( buffed_stats.mastery_value );

    profileset.output_data().versatility_rating( util::floor( player->composite_damage_versatility_rating() ) );
    profileset.output_data().versatility_pct( buffed_stats.damage_versatility );

    // tertiary stats

    profileset.output_data().avoidance_rating( player->composite_avoidance_rating() );
    profileset.output_data().avoidance_pct( buffed_stats.avoidance );
    profileset.output_data().leech_rating( player->composite_leech_rating() );
    profileset.output_data().leech_pct( buffed_stats.leech );
    profileset.output_data().speed_rating( player->composite_spell_haste_rating() );
    profileset.output_data().speed_pct( buffed_stats.run_speed );

    profileset.output_data().corruption( buffed_stats.corruption );
    profileset.output_data().corruption_resistance( buffed_stats.corruption_resistance );
  }
}

sim_control_t* filter_control( const sim_control_t* control )
{
  if ( control == nullptr )
  {
    return nullptr;
  }

  auto clone = new sim_control_t();

  for ( size_t i = 0, end = control -> options.size(); i < end; ++i )
  {
    auto pos = control -> options[ i ].name.find( "profileset" );
    if ( pos != 0 )
    {
      clone -> options.add( control -> options[ i ].scope,
                            control -> options[ i ].name,
                            control -> options[ i ].value );
    }
  }

  return clone;
}

} /* Namespace profileset ends */

#else

namespace profileset
{
profilesets_t::profilesets_t() {}
profilesets_t::~profilesets_t() {}
void create_options( sim_t* ) {}
sim_control_t* filter_control( const sim_control_t* ) { return nullptr; }
void profilesets_t::initialize( sim_t* ) {}
std::string profilesets_t::current_profileset_name() { return "DUMMY"; }
void profilesets_t::cancel() {}
bool profilesets_t::iterate( sim_t*  ) { return true ;}
void profilesets_t::output_html( const sim_t&, std::ostream& ) const {}
void profilesets_t::output_text( const sim_t&, std::ostream& ) const {}
size_t profilesets_t::n_profilesets() const { return 0; }
bool profilesets_t::is_running() const { return false; }
}

#endif
