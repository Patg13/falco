/* Copyright (C) 2019 Guilherme De Sena Brandine and
 *                    Andrew D. Smith
 * Authors: Guilherme De Sena Brandine, Andrew Smith
 *
 * This program is free software: you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 */

#include "Module.hpp"
#include <algorithm>
#include <iomanip>
#include <cmath>
#include <sstream>

using std::string;
using std::vector;
using std::array;
using std::unordered_map;
using std::sort;
using std::min;
using std::max;
using std::ostream;
using std::pair;
using std::transform;
using std::setprecision;
using std::runtime_error;
using std::make_pair;
using std::ostringstream;

/*****************************************************************************/
/******************* AUX FUNCTIONS *******************************************/
/*****************************************************************************/
// convert string to uppercase, to be used in making the short summaries where
// PASS, WARN and FAIL are uppercased
static string
toupper(const string &s) {
  string out;
  transform(s.begin(), s.end(), std::back_inserter(out),(int (*)(int))toupper);
  return out;
}


/*****************************************************************************/
/******************* IMPLEMENTATION OF FASTQC FUNCTIONS **********************/
/*****************************************************************************/
void
make_base_groups(vector<BaseGroup> &base_groups, 
                 const size_t &num_bases) {
  size_t starting_base = 0,
         end_base,
         interval = 1;

  base_groups.clear();
  for (; starting_base < num_bases;) {
    end_base = starting_base + interval - 1;
    if (end_base >= num_bases)
      end_base = num_bases;

    base_groups.push_back(BaseGroup(starting_base, end_base));
    starting_base += interval;
    if (starting_base == 9 && num_bases > 75)
      interval = 5;
    if (starting_base == 49 && num_bases > 200)
      interval = 10;
    if (starting_base == 99 && num_bases > 300)
      interval = 50;
    if (starting_base == 499 && num_bases > 1000)
      interval = 100;
    if (starting_base == 1000 && num_bases > 2000)
      interval = 500;
  }
}

void
make_default_base_groups(vector<BaseGroup> &base_groups,
                         const size_t &num_bases) {
  base_groups.clear();
  for (size_t i = 0; i < num_bases; ++i)
    base_groups.push_back(BaseGroup(i,i));
}

// FastQC extrapolation of counts to the full file size
double get_corrected_count(size_t count_at_limit,
                           size_t num_reads,
                           size_t dup_level,
                           size_t num_obs) {
  // See if we can bail out early
  if (count_at_limit == num_reads)
    return num_obs;

  // If there aren't enough sequences left to hide another sequence with this
  // count the we can also skip the calculation
  if (num_reads - num_obs < count_at_limit)
    return num_obs;

  // If not then we need to see what the likelihood is that we had
  // another sequence with this number of observations which we would
  // have missed. We'll start by working out the probability of NOT seeing a
  // sequence with this duplication level within the first count_at_limit
  // sequences of num_obs.  This is easier than calculating
  // the probability of seeing it.
  double p_not_seeing = 1.0;

  // To save doing long calculations which are never going to produce anything
  // meaningful we'll set a limit to our p-value calculation.  This is the
  // probability below which we won't increase our count by 0.01 of an
  // observation.  Once we're below this we stop caring about the corrected
  // value since it's going to be so close to the observed value thatwe can
  // just return that instead.
  double limit_of_caring = 1.0 - (num_obs/(num_obs + 0.01));
  for (size_t i = 0; i < count_at_limit; ++i) {
    p_not_seeing *= static_cast<double>((num_reads-i)-dup_level) /
                         static_cast<double>(num_reads-i);

    if (p_not_seeing < limit_of_caring) {
      p_not_seeing = 0;
      break;
    }
  }

  // Now we can assume that the number we observed can be
  // scaled up by this proportion
  return num_obs/(1 - p_not_seeing);
}

// Function to calculate the deviation of a histogram with 100 bins from a
// theoretical normal distribution with same mode and standard deviation
double
sum_deviation_from_normal(const array <double, 101> &gc_count,
                          array <double, 101> &theoretical) {
  /******************* BEGIN COPIED FROM FASTQC **********************/
  const size_t num_gc_bins = 101;

  // Sum of all gc counts in all histogram bins
  double total_count = 0.0;

  // We use the mode to calculate the theoretical distribution
  // so that we cope better with skewed distributions.
  size_t first_mode = 0;
  double mode_count = 0.0;

  for (size_t i = 0; i < num_gc_bins; ++i) {
    total_count += gc_count[i];
    if (gc_count[i] > mode_count) {
      mode_count = gc_count[i];
      first_mode = i;
    }
  }

  // The mode might not be a very good measure of the centre
  // of the distribution either due to duplicated vales or
  // several very similar values next to each other.  We therefore
  // average over adjacent points which stay above 95% of the modal
  // value

  double mode = 0;
  size_t mode_duplicates = 0;
  bool fell_off_top = true;

  for (size_t i = first_mode; i < num_gc_bins; ++i) {
    if (gc_count[i] > gc_count[first_mode] - (gc_count[first_mode]/10.0)) {
      mode += i;
      mode_duplicates++;
    }
    else {
      fell_off_top = false;
      break;
    }
  }

  bool fell_off_bottom = true;
  for (int i = first_mode - 1; i >= 0; --i) {
    if (gc_count[i] > gc_count[first_mode]
                          - (gc_count[first_mode]/10.0)) {
      mode += i;
      mode_duplicates++;
    }
    else {
      fell_off_bottom = false;
      break;
    }
  }

  if (fell_off_bottom || fell_off_top) {
    // If the distribution is so skewed that 95% of the mode
    // is off the 0-100% scale then we keep the mode as the
    // centre of the model
    mode = first_mode;
  } else {
    mode /= mode_duplicates;
  }

  // We can now work out a theoretical distribution
  double stdev = 0.0;
  for (size_t i = 0; i < num_gc_bins; ++i) {
    stdev += (i - mode) * (i - mode) * gc_count[i];
  }

  stdev = stdev / (total_count-1);
  stdev = sqrt(stdev);

  /******************* END COPIED FROM FASTQC **********************/
  // theoretical sampling from a normal distribution with mean = mode and stdev
  // = stdev to the mode from the sampled gc content from the data
  double ans = 0.0, theoretical_sum = 0.0, z;
  theoretical.fill(0);
  for (size_t i = 0; i <= 100; ++i) {
    z = i - mode;
    theoretical[i] = exp(- (z*z)/ (2.0 * stdev *stdev));
    theoretical_sum += theoretical[i];
  }

  // Normalize theoretical so it sums to the total of readsq
  for (size_t i = 0; i <= 100; ++i) {
    theoretical[i] = theoretical[i] * total_count / theoretical_sum;
  }

  for (size_t i = 0; i <= 100; ++i) {
    ans += fabs(gc_count[i] - theoretical[i]);
  }
  // Fractional deviation
  return 100.0 * ans / total_count;
}

/***************************************************************/
/********************* ABSTRACT MODULE *************************/
/***************************************************************/
Module::Module(const string &_module_name) : module_name(_module_name) {
  // make placeholders
  placeholder = module_name;

  // removes spaces
  placeholder.erase(remove_if(placeholder.begin(),
        placeholder.end(), isspace), placeholder.end());

  // lowercases it
  transform(placeholder.begin(),
            placeholder.end(), placeholder.begin(),
          [](unsigned char c){ return std::tolower(c); });

  // makes html placeholders
  placeholder_name = "{{" + placeholder + "name" + "}}";
  placeholder_data = "{{" + placeholder + "data" + "}}";
  placeholder_cs = "{{" + placeholder + "cs" + "}}";
  placeholder_ce = "{{" + placeholder + "ce" + "}}";
  placeholder_grade = "{{pass" + placeholder + "}}";
  grade = "pass";
  summarized = false;
}

Module::~Module() {

}

void
Module::write(ostream &os) {
  if (!summarized)
    throw runtime_error("Attempted to write module before summarizing : "
                        + module_name);
  os << ">>" << module_name << "\t" << grade << "\n";
  write_module(os);
  os << ">>END_MODULE\n";
}

void
Module::write_short_summary(ostream &os, const string &filename) {
  if (!summarized)
    throw runtime_error("Attempted to write module before summarizing : "
                        + module_name);
  os << toupper(grade) << "\t"
     << module_name << "\t"
     << filename << "\n";
}

// Guarantees the summarized flag is only set to true when module
// data has been summarized
void
Module::summarize(const FastqStats &stats) {
  summarize_module(stats);
  make_grade();
  html_data = make_html_data();
  summarized = true;
}

/***************************************************************/
/********************* SUMMARIZE FUNCTIONS *********************/
/***************************************************************/

/******************* BASIC STATISTICS **************************/
ModuleBasicStatistics::
ModuleBasicStatistics(const FalcoConfig &config)
: Module("Basic Statistics") {
    filename_stripped = config.filename_stripped;
}

void
ModuleBasicStatistics::summarize_module(const FastqStats &stats) {
  // Total sequences
  total_sequences = stats.num_reads;

  // min and max read length
  min_read_length = stats.min_read_length;
  max_read_length = stats.max_read_length;

  // These seem to always be the same on FastQC
  // File type
  file_type = "Conventional base calls";

  // File encoding
  file_encoding = "Sanger / Illumina 1.9";

  // Poor quality reads
  num_poor = 0;

  // Average read length
  avg_read_length = 0;
  size_t total_bases = 0;
  for (size_t i = 0; i < max_read_length; ++i) {
    if (i < FastqStats::kNumBases)
      total_bases += i * stats.read_length_freq[i];
    else
      total_bases += i * stats.long_read_length_freq[i - FastqStats::kNumBases];
  }

  avg_read_length = total_bases / total_sequences;

  // counts bases G and C in each base position
  avg_gc = 0;

  // GC %
  // GS: TODO delete gc calculation during stream and do it using the total G
  // counts in all bases
  avg_gc = 100 * stats.total_gc / static_cast<double>(total_bases);

}

// It's always a pass
void
ModuleBasicStatistics::make_grade() {
}

void
ModuleBasicStatistics::write_module(ostream &os) {
  os << "#Measure\tValue\n";
  os << "Filename\t" << filename_stripped << "\n";
  os << "File type\t" << file_type << "\n";
  os << "Encoding\t" << file_encoding << "\n";
  os << "Total Sequences\t" << total_sequences << "\n";
  os << "Sequences flagged as poor quality\t" << num_poor << "\n";
  os << "Sequence length\t";
  if (min_read_length == max_read_length) {
    os << min_read_length;
  } else {
    os << min_read_length << "-" << max_read_length;
  }
  os << "\n";
  os << "%GC\t" << static_cast<size_t>(avg_gc) << "\n";
}

string
ModuleBasicStatistics::make_html_data() {
  ostringstream data;
  data << "<table><thead><tr><th>Measure</th><th>Value"
       << "</th></tr></thead><tbody>";
  data << "<tr><td>Filename</td><td>" << filename_stripped
       << "</td></tr>";
  data << "<tr><td>File type</td><td>" << file_type
       << "</td></tr>";
  data << "<tr><td>Encoding</td><td>" << file_encoding
       << "</td></tr>";
  data << "<tr><td>Total Sequences</td><td>" << total_sequences << "</td></tr>";
  data << "<tr><td>Sequences Flagged As Poor Quality</td><td>"
       << num_poor << "</td></tr>";
  data << "<tr><td>Sequence length</td><td>";
  if (min_read_length != max_read_length) {
    data << min_read_length << " - " << max_read_length;
  }
  else {
    data << max_read_length;
  }
  data << "</td></tr>";
  data << "<tr><td>%GC:</td><td>" << avg_gc << "</td></tr>";
  data << "</tbody></table>";
  return data.str();
}

/******************* PER BASE SEQUENCE QUALITY **********************/
ModulePerBaseSequenceQuality::ModulePerBaseSequenceQuality
(const FalcoConfig &config): Module("Per base sequence quality") {
  auto base_lower = config.limits.find("quality_base_lower");
  auto base_median = config.limits.find("quality_base_median");

  base_lower_warn = (base_lower->second).find("warn")->second;
  base_lower_error = (base_lower->second).find("error")->second;
  base_median_warn = (base_median->second).find("warn")->second;
  base_median_error = (base_median->second).find("error")->second;

  do_group = !config.nogroup;
}

void
ModulePerBaseSequenceQuality::summarize_module(const FastqStats &stats) {
  // Quality quantiles for base positions
  double ldecile_thresh,
         lquartile_thresh,
         median_thresh,
         uquartile_thresh,
         udecile_thresh;

  size_t cur_ldecile = 0,
         cur_lquartile = 0,
         cur_median = 0,
         cur_uquartile = 0,
         cur_udecile = 0;

  size_t cur;
  size_t cur_sum;
  size_t counts;
  double cur_mean;
  num_bases = stats.max_read_length;

  // first, do the groups
  if (do_group) make_base_groups(base_groups, num_bases);
  else make_default_base_groups(base_groups, num_bases);
  num_groups = base_groups.size();

  // Reserves space I know I will use
  group_mean = vector<double>(num_groups, 0.0);
  group_ldecile = vector<size_t>(num_groups, 0);
  group_lquartile = vector<size_t>(num_groups, 0);
  group_median = vector<size_t>(num_groups, 0);
  group_uquartile = vector<size_t>(num_groups, 0);
  group_udecile = vector<size_t>(num_groups, 0);

  // temp
  vector<size_t>histogram(128, 0);
  size_t bases_in_group = 0;

  for (size_t group = 0; group < num_groups; ++group) {
    // Find quantiles for each base group
    for (size_t i = base_groups[group].start;
              i  <= base_groups[group].end; ++i) {

      // reset group values
      bases_in_group = 0;
      for (size_t j = 0; j < 128; ++j)
        histogram[j] = 0;

      for (size_t j = 0; j < FastqStats::kNumQualityValues; ++j) {
        // get value
        if (i < FastqStats::kNumBases) {
          cur = stats.position_quality_count[
            (i << FastqStats::kBitShiftQuality) | j];
        }
        else {
          cur = stats.long_position_quality_count[
            ((i - FastqStats::kNumBases) << FastqStats::kBitShiftQuality) | j];
        }

        // Add to Phred histogram
        histogram[j] += cur;
      }

      // Number of bases seen in position i
      if (i < FastqStats::kNumBases) {
        bases_in_group += stats.cumulative_read_length_freq[i];
      } else {
        bases_in_group +=
          stats.long_cumulative_read_length_freq[i - FastqStats::kNumBases];
      }
    }
    ldecile_thresh = 0.1 * bases_in_group;
    lquartile_thresh = 0.25 * bases_in_group;
    median_thresh = 0.5 * bases_in_group;
    uquartile_thresh = 0.75 * bases_in_group;
    udecile_thresh = 0.9 * bases_in_group;

    // now go again through the counts in each quality value to find the
    // quantiles
    cur_sum = 0;
    counts = 0;

    for (size_t j = 0; j < FastqStats::kNumQualityValues; ++j) {
      // Finds in which bin of the histogram reads are
      cur = histogram[j];
      if (counts < ldecile_thresh && counts + cur >= ldecile_thresh)
        cur_ldecile = j;
      if (counts < lquartile_thresh && counts + cur >= lquartile_thresh)
        cur_lquartile = j;
      if (counts < median_thresh && counts + cur >= median_thresh)
        cur_median = j;
      if (counts < uquartile_thresh && counts + cur >= uquartile_thresh)
        cur_uquartile = j;
      if (counts < udecile_thresh && counts + cur >= udecile_thresh)
        cur_udecile = j;
      cur_sum += cur*j;
      counts += cur;
    }

    cur_mean = static_cast<double>(cur_sum) /
               static_cast<double>(bases_in_group);

    group_mean[group] = cur_mean;
    group_ldecile[group] = cur_ldecile;
    group_lquartile[group] = cur_lquartile;
    group_median[group] = cur_median;
    group_uquartile[group] = cur_uquartile;
    group_udecile[group] = cur_udecile;
  }
}

void
ModulePerBaseSequenceQuality::make_grade() {
  num_warn = 0;
  num_error = 0;
  for (size_t i = 0; i < num_groups; ++i) {
    if (grade != "fail") {
      if (group_lquartile[i] < base_lower_error ||
          group_median[i] < base_median_error) {
        num_error++;
      } else if (group_lquartile[i] < base_lower_warn ||
                 group_median[i] < base_median_warn) {
        num_warn++;
      }
    }
  }

  // bad bases greater than 25% of all bases
  if (num_error > 0)
    grade = "fail";
  else if (num_warn > 0)
    grade = "warn";
}

void
ModulePerBaseSequenceQuality::write_module(ostream &os) {
  os << "#Base\tMean\tMedian\tLower Quartile\tUpper Quartile" <<
        "\t10th Percentile\t90th Percentile\n";

  // GS: TODO make base groups
  for (size_t i = 0; i < num_groups; ++i) {
      if(base_groups[i].start == base_groups[i].end)
        os << base_groups[i].start + 1 << "\t";
      else
        os << base_groups[i].start + 1 << "-" << base_groups[i].end + 1 << "\t";

      os << group_mean[i] << "\t"
         << group_median[i] << ".0\t"
         << group_lquartile[i] << ".0\t"
         << group_uquartile[i] << ".0\t"
         << group_ldecile[i] << ".0\t"
         << group_udecile[i] << ".0\n";
  }
}

// Plotly data
string
ModulePerBaseSequenceQuality::make_html_data() {
  ostringstream data;
  for (size_t i = 0; i < num_groups; ++i) {
    data << "{y : [";

      data << group_ldecile[i] << ", "
           << group_lquartile[i] << ", "
           << group_median[i] << ", "
           << group_uquartile[i] << ", "
           << group_udecile[i] << "], ";
    data << "type : 'box', name : ' ";
    if (base_groups[i].start == base_groups[i].end)
      data << base_groups[i].start + 1;
    else
      data << base_groups[i].start + 1 << "-" << base_groups[i].end + 1;
    data << "bp', ";
    data << "marker : {color : '";

    // I will color the boxplot based on whether it passed or failed
    if (group_median[i] < base_median_error ||
        group_lquartile[i] < base_lower_error)
      data << "red";
    else if (group_median[i] < base_median_warn || 
             group_lquartile[i] < base_lower_warn)
      data << "yellow";
    else
      data << "green";
    data << "'}}";
    if (i < num_bases - 1) {
      data << ", ";
    }
  }
  return data.str();
}

/************** PER TILE SEQUENCE QUALITY ********************/
ModulePerTileSequenceQuality::
ModulePerTileSequenceQuality(const FalcoConfig &config) :
Module("Per tile sequence quality") {
  auto grade_tile = config.limits.find("tile")->second;
  grade_warn = grade_tile.find("warn")->second;
  grade_error = grade_tile.find("error")->second;
}

void
ModulePerTileSequenceQuality::summarize_module(const FastqStats &stats) {
  max_read_length = stats.max_read_length;
  tile_position_quality = stats.tile_position_quality;
  // First I calculate the number of counts for each position
  vector<size_t> position_counts(max_read_length, 0);

  for (auto v : stats.tile_position_quality) {
    for (size_t i = 0; i < v.second.size(); ++i) {
      position_counts[i] +=
        stats.tile_position_count.find(v.first)->second[i];
    }
  }
   
  // Now I calculate the sum of all tile qualities in each position
  vector<double> mean_in_base(max_read_length, 0.0);
  for (auto v : tile_position_quality) {
    for(size_t i = 0; i < v.second.size(); ++i) {
      mean_in_base[i] += v.second[i];
    }
  }

  // Now transform sum into mean
  for (size_t i = 0; i < max_read_length; ++i) {
    mean_in_base[i] = mean_in_base[i] / position_counts[i];
  }

  for (auto &v : tile_position_quality) {
    for (size_t i = 0; i < v.second.size(); ++i) {
      // transform sum of all qualities in mean
      v.second[i] = v.second[i] / 
        stats.tile_position_count.find(v.first)->second[i];
      
      // subtract the global mean
      v.second[i] -= mean_in_base[i];
    }
  }
  // sorts tiles
  tiles_sorted.clear();
  for (auto v : tile_position_quality) {
    tiles_sorted.push_back(v.first);
  }

  sort(tiles_sorted.begin(), tiles_sorted.end());

}

void
ModulePerTileSequenceQuality::make_grade() {
  grade = "pass";
  for (auto &v : tile_position_quality) {
    for (size_t i = 0; i < v.second.size(); ++i) {
      if (grade != "fail") {
        if (v.second[i] <= -grade_error) {
          grade = "fail";
        }
        else if (v.second[i] <= -grade_warn) {
          grade = "warn";
        }
      }
    }
  }
}

void
ModulePerTileSequenceQuality::write_module(ostream &os) {

  // prints tiles sorted by value
  os << "#Tile\tBase\tMean\n";
  for (size_t i = 0; i < tiles_sorted.size(); ++i) {
    for (size_t j = 0; j < max_read_length; ++j) {

      if (tile_position_quality[tiles_sorted[i]].size() >= j) {
        os << tiles_sorted[i] << "\t" << j + 1 << "\t"
           << tile_position_quality[tiles_sorted[i]][j];
        os << "\n";
      }
    }
  }
}

string
ModulePerTileSequenceQuality::make_html_data() {
  ostringstream data;
  data << "{x : [";
  for (size_t i = 0; i < max_read_length; ++i) {
    data << i+1;
    if (i < max_read_length - 1)
      data << ",";
  }

  // Y : Tile
  data << "], y: [";
  bool first_seen = false;
  for (size_t i = 0; i < tiles_sorted.size(); ++i) {
    if (!first_seen) first_seen = true;
    else data << ",";
    data << tiles_sorted[i];
  }

  // Z: quality z score
  data << "], z: [";
  first_seen = false;
  for (size_t i = 0; i < tiles_sorted.size(); ++i) {
    if (!first_seen) first_seen = true;
    else data << ", ";

    // start new array with all counts
    data << "[";
    for (size_t j = 0; j < max_read_length; ++j) {
      data << tile_position_quality[tiles_sorted[i]][j];
      if (j < max_read_length - 1) data << ",";
    }
    data << "]";
  }
  data << "]";
  data << ", type : 'heatmap' }";

  return data.str();
}

/******************* PER SEQUENCE QUALITY SCORE **********************/
ModulePerSequenceQualityScores::
ModulePerSequenceQualityScores(const FalcoConfig &config) :
Module("Per sequence quality scores") {
  mode_val = 0;
  mode_ind = 0;

  auto mode_limits = config.limits.find("quality_sequence");
  mode_warn = (mode_limits->second).find("warn")->second;
  mode_error = (mode_limits->second).find("error")->second;
}

void
ModulePerSequenceQualityScores::summarize_module(const FastqStats &stats) {
  // Need to copy this to write later
  quality_count = stats.quality_count;

  // get mode for grade
  for (size_t i = 0; i < FastqStats::kNumQualityValues; ++i) {
    if (stats.quality_count[i] > mode_val) {
      mode_val = stats.quality_count[i];
      mode_ind = i;
    }
  }
}

void
ModulePerSequenceQualityScores::make_grade() {
  if (mode_ind < mode_warn) {
    grade = "warn";
  }

  if (mode_ind < mode_error) {
    grade = "fail";
  }
}

void
ModulePerSequenceQualityScores::write_module(ostream &os) {
  os << "#Quality\tCount\n";
  for (size_t i = 0; i < FastqStats::kNumQualityValues; ++i) {
    if (quality_count[i] > 0)
      os << i << "\t" << quality_count[i] << "\n";
  }
}

string
ModulePerSequenceQualityScores::make_html_data() {
  ostringstream data;
  data << "{x : [";
  bool seen_first = false;
  for (size_t i = 0; i < 41; ++i) {
    if (seen_first)
      data << ",";
    else
      seen_first = true;

    if (quality_count[i] > 0)
      data << i;
  }

  // Y values: frequency with which they were seen
  data << "], y : [";
  seen_first = false;
  for (size_t i = 0; i < 41; ++i) {
    if (seen_first)
      data << ",";
    else
      seen_first = true;

    if (quality_count[i] > 0)
      data << quality_count[i];
  }
  data << "], type: 'line', line : {color : 'red'}, "
       << "name : 'Sequence quality distribution'}";

  return data.str();
}

/******************* PER BASE SEQUENCE CONTENT **********************/
ModulePerBaseSequenceContent::
ModulePerBaseSequenceContent(const FalcoConfig &config) :
Module("Per base sequence content") {
  auto sequence_limits = config.limits.find("sequence")->second;
  sequence_warn = sequence_limits.find("warn")->second;
  sequence_error = sequence_limits.find("error")->second;
}

void
ModulePerBaseSequenceContent::summarize_module(const FastqStats &stats) {
  double a, t, g, c, n;
  double total; //a+c+t+g+n
  max_diff = 0.0;

  num_bases = stats.max_read_length;
  a_pct = vector<double>(num_bases, 0.0);
  c_pct = vector<double>(num_bases, 0.0);
  t_pct = vector<double>(num_bases, 0.0);
  g_pct = vector<double>(num_bases, 0.0);
  for (size_t i = 0; i < num_bases; ++i) {
    if (i < FastqStats::kNumBases) {
      a = stats.base_count[(i << FastqStats::kBitShiftNucleotide)];
      c = stats.base_count[(i << FastqStats::kBitShiftNucleotide) | 1];
      t = stats.base_count[(i << FastqStats::kBitShiftNucleotide) | 2];
      g = stats.base_count[(i << FastqStats::kBitShiftNucleotide) | 3];
      n = stats.n_base_count[i];
    } else {
      a = stats.long_base_count[
            ((i - FastqStats::kNumBases) << FastqStats::kBitShiftNucleotide)
          ];
      c = stats.long_base_count[
            ((i - FastqStats::kNumBases) << FastqStats::kBitShiftNucleotide) | 1
          ];
      t = stats.long_base_count[
            ((i - FastqStats::kNumBases) << FastqStats::kBitShiftNucleotide) | 2
          ];
      g = stats.long_base_count[
            ((i - FastqStats::kNumBases) << FastqStats::kBitShiftNucleotide) | 3
          ];
      n = stats.long_n_base_count[
              i - FastqStats::kNumBases
          ];
    }

    // turns above values to percent
    total = static_cast<double>(a + c + t + g + n);
    a = 100.0*a / total;
    c = 100.0*c / total;
    t = 100.0*t / total;
    g = 100.0*g / total;
    g_pct[i] = g;
    a_pct[i] = a;
    t_pct[i] = t;
    c_pct[i] = c;

    max_diff = max(max_diff, fabs(a-c));
    max_diff = max(max_diff, fabs(a-t));
    max_diff = max(max_diff, fabs(a-g));
    max_diff = max(max_diff, fabs(c-t));
    max_diff = max(max_diff, fabs(c-g));
    max_diff = max(max_diff, fabs(t-g));
  }
}

void
ModulePerBaseSequenceContent::make_grade() {
  if (max_diff > sequence_error) {
    grade = "fail";
  }
  else if (max_diff > sequence_warn) {
    grade = "warn";
  }
}

void
ModulePerBaseSequenceContent::write_module(ostream &os) {
  os << "#Base\tG\tA\tT\tC\n";
  for (size_t i = 0; i < num_bases; ++i) {
    os << i+1 << "\t" <<
          g_pct[i] << "\t" <<
          a_pct[i] << "\t" <<
          t_pct[i] << "\t" <<
          c_pct[i] << "\n";
  }
}

string
ModulePerBaseSequenceContent::make_html_data() {
  ostringstream data;
  // ATGC
  for (size_t base = 0; base < 4; ++base) {
    // start line
    data << "{";

    // X values : base position
    data << "x : [";
    for (size_t i = 0; i < num_bases; ++i) {
      data << i+1;
      if (i < num_bases - 1)
        data << ", ";
    }

    // Y values: frequency with which they were seen
    data << "], y : [";
    for (size_t i = 0; i < num_bases; ++i) {
      if (base == 0) data << a_pct[i];
      if (base == 1) data << c_pct[i];
      if (base == 2) data << t_pct[i];
      if (base == 3) data << g_pct[i];
      if (i < num_bases - 1)
        data << ", ";
    }
    data << "], mode : 'lines', name : '" + size_t_to_seq(base, 1) + "', ";

    // color
    data << "line :{ color : '";
    if (base == 0)
      data << "green";
    else if (base == 1)
      data << "blue";
    else if (base == 2)
      data << "red";
    else
      data << "black";
    data << "'}";
    // end color

    // end line
    data << "}";
    if (base < 4)
      data << ", ";
  }

  return data.str();
}

/******************* PER SEQUENCE GC CONTENT *****************/
ModulePerSequenceGCContent::
ModulePerSequenceGCContent(const FalcoConfig &config) :
Module("Per sequence GC content") {
  auto gc_vars = config.limits.find("gc_sequence")->second;
  gc_warn = gc_vars.find("warn")->second;
  gc_error = gc_vars.find("error")->second;
}

void
ModulePerSequenceGCContent::summarize_module(const FastqStats &stats) {
  gc_count = stats.gc_count;
  gc_deviation = sum_deviation_from_normal(gc_count,
                                           theoretical_gc_count);
}

void
ModulePerSequenceGCContent::make_grade() {
  if (gc_deviation >= gc_error) {
    grade = "fail";
  }
  else if (gc_deviation >= gc_warn) {
    grade = "warn";
  }
}

void
ModulePerSequenceGCContent::write_module(ostream &os) {
  os << "#GC Content\tCount\n";
  for (size_t i = 0; i <= 100; ++i) 
    os << i << "\t" << gc_count[i] << "\n";
}

string
ModulePerSequenceGCContent::make_html_data() {
  ostringstream data;
  // Actual count
  data << "{x : [";
  for (size_t i = 0; i < 101; ++i) {
    data << i + 1;
    if (i < 101)
      data << ", ";
  }

  // Y values: frequency with which they were seen
  data << "], y : [";
  for (size_t i = 0; i < 101; ++i) {
    data << gc_count[i];
    if (i < 101)
      data << ", ";
  }
  data << "], type: 'line', line : {color : 'red'},name : 'GC distribution'}";

  // Theoretical count
  data << ", {x : [";
  for (size_t i = 0; i < 101; ++i) {
    data << i + 1;
    if (i < 101)
      data << ", ";
  }

  // Y values: frequency with which they were seen
  data << "], y : [";
  for (size_t i = 0; i < 101; ++i) {
    data << theoretical_gc_count[i];
    if (i < 101)
      data << ", ";
  }
  data << "], type: 'line', line : {color : 'blue'},"
       << "name : 'Theoretical distribution'}";

  return data.str();
}

/******************* PER BASE N CONTENT **********************/
ModulePerBaseNContent::
ModulePerBaseNContent(const FalcoConfig &config) :
Module("Per base N content") {
  auto grade_n = config.limits.find("n_content")->second;
  grade_n_warn = grade_n.find("warn")->second;
  grade_n_error = grade_n.find("error")->second;
}

void
ModulePerBaseNContent::summarize_module(const FastqStats &stats) {
  num_bases = stats.max_read_length;
  n_pct = vector<double>(num_bases, 0.0);
  for (size_t i = 0; i < num_bases; ++i) {
    if (i < FastqStats::kNumBases) {
      n_pct[i] = 100.0 * stats.n_base_count[i] /
                         stats.cumulative_read_length_freq[i];
    }
    else {
      n_pct[i] = 100.0 * stats.long_n_base_count[i - FastqStats::kNumBases] /
                         stats.long_cumulative_read_length_freq[i];
    }
  }
}

void
ModulePerBaseNContent::make_grade() {
  for (size_t i = 0; i < num_bases; ++i) {
    if(grade != "fail") {
      if (n_pct[i] > grade_n_error) {
        grade = "fail";
      }

      else if (n_pct[i] > grade_n_warn) {
        grade = "warn";
      }
    }
  }
}

void
ModulePerBaseNContent::write_module(ostream &os) {
  os << "#Base\tN-Count\n";
  for (size_t i = 0; i < num_bases; ++i) {
      os << i+1 << "\t" << n_pct[i] << "\n";
  }
}

string
ModulePerBaseNContent::make_html_data() {
  ostringstream data;
  // base position
  data << "{x : [";
  for (size_t i = 0; i < num_bases; ++i) {
    data << i + 1;
    if (i < num_bases - 1)
      data << ", ";
  }

  // Y values: frequency with which they were seen
  data << "], y : [";
  for (size_t i = 0; i < num_bases; ++i) {
    data << n_pct[i];

    if (i < num_bases - 1)
      data << ", ";
  }
  data << "], type: 'line', line : {color : 'red'}, "
       << "name : 'Fraction of N reads per base'}";

  return data.str();
}

/************** SEQUENCE LENGTH DISTRIBUTION *****************/
ModuleSequenceLengthDistribution::
ModuleSequenceLengthDistribution(const FalcoConfig &config) :
Module("Sequence Length Distribution") {
  auto length_grade = config.limits.find("sequence_length")->second;
  do_grade_error = (length_grade.find("error")->second != 0);
  do_grade_warn = (length_grade.find("warn")->second != 0);
}

void
ModuleSequenceLengthDistribution::summarize_module(const FastqStats &stats) {
  max_read_length = stats.max_read_length;

  has_empty_read = (stats.min_read_length == 0);
  is_all_same_length = true;
  // store the read lengths
  sequence_lengths = vector<size_t>(max_read_length, 0);

  size_t num_nonzero = 0;
  for (size_t i = 0; i < max_read_length; ++i) {
    if (i < FastqStats::kNumBases) {
      sequence_lengths[i] = stats.read_length_freq[i];
    } else {
      sequence_lengths[i] = stats.long_read_length_freq[
                              i - FastqStats::kNumBases
                            ];
    }

    if (sequence_lengths[i] > 0) {
      num_nonzero++;
      if (num_nonzero > 1)
        is_all_same_length = false;
    }
  }
}

void
ModuleSequenceLengthDistribution::make_grade() {
  if (do_grade_warn) {
    if (!is_all_same_length) {
      grade = "warn";
    }
  }
  if (do_grade_error) {
    if (has_empty_read) {
      grade = "fail";
    }
  }
}

void
ModuleSequenceLengthDistribution::write_module(ostream &os) {
  os << "Length\tCount\n";
  for (size_t i = 0; i < max_read_length; ++i) {
    if (sequence_lengths[i] > 0) {
      os << i+1 << "\t" << sequence_lengths[i] << "\n";
    }
  }
}

string
ModuleSequenceLengthDistribution::make_html_data() {
  ostringstream data;
  // X values : avg quality phred scores
  data << "{x : [";
  bool first_seen = false;
  for (size_t i = 0; i < max_read_length; ++i) {
    if (sequence_lengths[i] > 0) {
      if (first_seen)
        data << ",";
      data << "\"" << i+1 << " bp\"";
      first_seen = true;
    }
  }

  // Y values: frequency with which they were seen
  data << "], y : [";
  first_seen = false;
  for (size_t i = 0; i < max_read_length; ++i) {
    if (sequence_lengths[i] > 0) {
      if (first_seen)
        data << ",";
      data << sequence_lengths[i];
      first_seen = true;
    }
  }

  // Put the sequence value in the text
  data << "], text : [";
  first_seen = false;
  for (size_t i = 0; i < max_read_length; ++i) {
    if (sequence_lengths[i] > 0) {
      if (first_seen) 
        data << ",";
      data << i+1;
      first_seen = true;
    }
  }

  data << "], type: 'bar', marker : {color : 'rgba(55,128,191,1.0)',"
       << "line : {width : 2}}, "
       << "name : 'Sequence length distribution'}";

  return data.str();
}

/************** DUPLICATE SEQUENCES **************************/
ModuleSequenceDuplicationLevels::
ModuleSequenceDuplicationLevels(const FalcoConfig &config) :
Module("Sequence Duplication Levels") {
  percentage_deduplicated.fill(0);
  percentage_total.fill(0);
  auto grade_dup = config.limits.find("duplication")->second;
  grade_dup_warn = grade_dup.find("warn")->second;
  grade_dup_error = grade_dup.find("error")->second;
}

void
ModuleSequenceDuplicationLevels::summarize_module(const FastqStats &stats) {
    seq_total = 0.0;
    seq_dedup = 0.0;

    // Key is frequenccy (r), value is number of times we saw a sequence
    // with that frequency (Nr)
    for (auto v : stats.sequence_count) {
      if (counts_by_freq.count(v.second) == 0) {
        counts_by_freq[v.second] = 0;
      }
      counts_by_freq[v.second]++;
    }

    // Now we change it to the FastQC corrected extrapolation
    for (auto v : counts_by_freq) {
      counts_by_freq[v.first] =
      get_corrected_count(stats.count_at_limit, stats.num_reads,
                          v.first, v.second);
    }

    // Group in blocks similarly to fastqc
    for (auto v : counts_by_freq) {
      size_t dup_slot = v.first - 1;
      if (v.first >= 10000) dup_slot = 15;
      else if (v.first >= 5000) dup_slot = 14;
      else if (v.first >= 1000) dup_slot = 13;
      else if (v.first >= 500) dup_slot = 12;
      else if (v.first >= 100) dup_slot = 11;
      else if (v.first >= 50) dup_slot = 10;
      else if (v.first >= 10) dup_slot = 9;

      percentage_deduplicated[dup_slot] += v.second;
      percentage_total[dup_slot] += v.second * v.first;

      seq_total += v.second * v.first;
      seq_dedup += v.second;
    }

    // "Sequence duplication estimate" in the summary
    total_deduplicated_pct = 100.0 * seq_dedup / seq_total;

    // Convert to percentage
    for (auto &v : percentage_deduplicated)
      v = 100.0 * v / seq_dedup;  // Percentage of unique sequences in bin

     // Convert to percentage
    for (auto &v : percentage_total)
      v = 100.0 * v / seq_total;  // Percentage of sequences in bin
}

void
ModuleSequenceDuplicationLevels::make_grade() {
  // pass warn fail criteria : unique reads must be >80%
  // (otherwise warn) or >50% (otherwisefail)
  if (total_deduplicated_pct <= grade_dup_error) {
    grade = "fail";
  }
  else if (total_deduplicated_pct <= grade_dup_warn) {
    grade = "warn";
  }
}

void
ModuleSequenceDuplicationLevels::write_module(ostream &os) {
  os << "#Total Deduplicated Percentage\t" <<
         total_deduplicated_pct << "\n";

  os << "#Duplication Level\tPercentage of deduplicated\t"
     << "Percentage of total\n";

  for (size_t i = 0; i < 9; ++i) {
    os << i+1 << "\t" << percentage_deduplicated[i] << "\t"
       << percentage_total[i] << "\n";
  }

  os << ">10\t" << percentage_deduplicated[9]
     << "\t" << percentage_total[9] << "\n";
  os << ">50\t" << percentage_deduplicated[10]
     << "\t" << percentage_total[10] << "\n";
  os << ">100\t" << percentage_deduplicated[11]
     << "\t" << percentage_total[11] << "\n";
  os << ">500\t" << percentage_deduplicated[12]
     << "\t" << percentage_total[12] << "\n";
  os << ">1k\t" << percentage_deduplicated[13]
     << "\t" << percentage_total[13] << "\n";
  os << ">5k\t" << percentage_deduplicated[14]
     << "\t" << percentage_total[14] << "\n";
  os << ">10k+\t" << percentage_deduplicated[15]
     << "\t" << percentage_total[15] << "\n";
}

string
ModuleSequenceDuplicationLevels::make_html_data() {
  ostringstream data;
  // non-deduplicated
  data << "{x : [1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16]";

  // total percentage in each bin
  data << ", y : [";
  for (size_t i = 0; i < 16; ++i) {
    data << percentage_total[i];

    if (i < 15)
      data << ", ";
  }
  data << "], type: 'line', line : {color : 'blue'}, "
       << "name : 'total sequences'}";

  // deduplicated
  data << ", {x : [1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16]";


  // total percentage in deduplicated
  data << ", y : [";
  for (size_t i = 0; i < 16; ++i) {
    data << percentage_deduplicated[i];

    if (i < 15)
      data << ", ";
  }
  data << "], type: 'line', line : {color : 'red'}, "
       << "name : 'deduplicated sequences'}";

  return data.str();
}


/************** OVERREPRESENTED SEQUENCES ********************/

ModuleOverrepresentedSequences::
ModuleOverrepresentedSequences(const FalcoConfig &config) :
Module("Overrepresented sequences") {
  auto grade_overrep = config.limits.find("overrepresented")->second;
  grade_warn = grade_overrep.find("warn")->second;
  grade_error = grade_overrep.find("error")->second;
}

string
ModuleOverrepresentedSequences::get_matching_contaminant (const string &seq) {
  size_t best = 0;
  string ret;
  for (auto v : contaminants) {
    if (seq.size() > v.second.size()) {
      // contaminant contained in sequence
      if (seq.find(v.second) != string::npos) {
        if (v.second.size() > best) {
          best = v.second.size();
          ret = v.first;
        }
      }
    } else {
      // sequence contained in contaminant
      if (v.second.find(seq) != string::npos) {
        // In this case this is the best possible match so return it
        return v.first;
      }
    }
  }

  // If any sequence is a match, return the best one
  if (best > 0)
    return ret;
  return "No Hit";
}

void
ModuleOverrepresentedSequences::summarize_module(const FastqStats &stats) {
  // Keep only sequences that pass the input cutoff
  num_reads = stats.num_reads;
  for (auto it = stats.sequence_count.begin();
            it != stats.sequence_count.end(); ++it) {
    if (it->second > num_reads * min_fraction_to_overrepresented) {
      overrep_sequences.push_back(*it);
    }
  }

  // Sort strings by frequency
  sort(begin(overrep_sequences), end(overrep_sequences),
       [](pair<string, size_t> &a, pair<string, size_t> &b){
         return a.second > b.second;
       });
}

void
ModuleOverrepresentedSequences::make_grade() {
  for (auto seq : overrep_sequences) {
    // implment pass warn fail for overrep sequences
    if (grade != "fail") {
      // get percentage that overrep reads represent
      double pct = 100.0 * seq.second / num_reads;
      if (pct > grade_error) {
        grade = "fail";
      }
      else if (pct > grade_warn) {
        grade = "warn";
      }
    }
  }
}

void
ModuleOverrepresentedSequences::write_module(ostream &os) {
  os << "#Sequence\tCount\tPercentage\tPossible Source\n";
  for (auto seq : overrep_sequences) {
      os << seq.first << "\t" << seq.second <<  "\t" <<
        100.0 * seq.second / num_reads << "\t"
        << get_matching_contaminant(seq.first) << "\n";
  }
}

string
ModuleOverrepresentedSequences::make_html_data() {
  ostringstream data;
  // Header
  data << "<table><thead><tr>";
  data << "<th>Sequence</th>";
  data << "<th>Count</th>";
  data << "<th>Percentage</th>";
  data << "<th>Possible Source</th>";
  data << "</tr></thead><tbody>";

  // All overrep sequences
  for (auto v : overrep_sequences) {
    data << "<tr><td>" << v.first << "</td>";
    data << "<td>" << v.second << "</td>";
    data << "<td>" << 100.0 * v.second / num_reads << "</td>";
    data << "<td>" << get_matching_contaminant(v.first)
         << "</td>";
    data << "</tr>";
  }
  data << "</tbody></table>";

  return data.str();
}


/************** ADAPTER CONTENT ***********/
ModuleAdapterContent::
ModuleAdapterContent(const FalcoConfig &config) :
Module("Adapter Content") {
  // pass the file read in config to adapters
  adapters = config.adapters;
  num_bases_counted = FastqStats::kNumBases;
  // maximum adapter % before pass/warn/fail
  auto grade_adapter = config.limits.find("adapter")->second;
  grade_warn = grade_adapter.find("warn")->second;
  grade_error = grade_adapter.find("error")->second;
}

void
ModuleAdapterContent::summarize_module(const FastqStats &stats) {
  // get matrices from stats
  kmer_count = stats.kmer_count;
  pos_kmer_count = stats.pos_kmer_count;

  // do not bother with 500 bases if the max read length does not go as far
  if (stats.max_read_length < num_bases_counted)
    num_bases_counted = stats.max_read_length;

  size_t kmer_pos_index;
  size_t adapter_sevenmer;

  // First we count cumulatively the sevenmer positions
  for (size_t i = 0; i < num_bases_counted; ++i) {
    // in the first position we just allocate
    if (i == 0) kmer_by_base[i] = vector<double> (adapters.size(), 0.0);

    // otherwise we make it cumulatie by copying the previous position
    else kmer_by_base[i] = vector<double> (kmer_by_base[i-1].begin(),
                                           kmer_by_base[i-1].end());

    for (size_t which_adapter = 0; which_adapter < adapters.size(); 
        ++which_adapter) {
      adapter_sevenmer = adapters[which_adapter].second;

      // get the index corresponding to the base position and adapter kmer
      kmer_pos_index = (i << FastqStats::kBitShiftKmer) | adapter_sevenmer;
      kmer_by_base[i][which_adapter] += kmer_count[kmer_pos_index];
    }
  }
  
  // Now we turn the counts into percentages
  for (size_t i = 0; i < num_bases_counted; ++i) {
    for (size_t which_adapter = 0; which_adapter < adapters.size(); 
        ++which_adapter) {
      if (pos_kmer_count[i] > 0) {
        kmer_by_base[i][which_adapter] *= 100.0;
        kmer_by_base[i][which_adapter] /= static_cast<double>(pos_kmer_count[i]);
      } else {
        kmer_by_base[i][which_adapter] = 0;
      }
    }
  }
}

void
ModuleAdapterContent::make_grade() {
  for (size_t i = 0; i < num_bases_counted; ++i) {
    for (size_t which_adapter = 0; which_adapter < adapters.size();
        ++which_adapter) {
      if (grade != "fail") {
        if (kmer_by_base[i][which_adapter] > grade_error) {
          grade = "fail";
        } else if (kmer_by_base[i][which_adapter] > grade_warn) {
          grade = "warn";
        }
      }
    }
  }
}

void
ModuleAdapterContent::write_module(ostream &os) {
  os << "#Position";

  // adapter names
  for (size_t which_adapter = 0; which_adapter < adapters.size();
      ++which_adapter) {
    os << "\t" << adapters[which_adapter].first;
  }
  os << "\n";

  for (size_t i = 0; i < num_bases_counted; ++i) {
    os << i + 1;
    for (size_t which_adapter = 0; which_adapter < adapters.size();
        ++which_adapter) {
      os << "\t" << kmer_by_base[i][which_adapter];
    }
    os << "\n";
  }
}

string
ModuleAdapterContent::make_html_data() {
  bool seen_first = false;
  ostringstream data;
  for (size_t which_adapter = 0; which_adapter < adapters.size();
      ++which_adapter) {
    if (!seen_first) {
      seen_first = true;
    }
    else {
      data << ",";
    }
    data << "{";

    // X values : read position
    data << "x : [";
    for (size_t i = 0; i < num_bases_counted; ++i) {
        data << i+1;
        if (i < num_bases_counted - 1) data << ",";
    }
    data << "]";

    // Y values : cumulative adapter frequency
    data << ", y : [";
    for (size_t i = 0; i < num_bases_counted; ++i) {
      data << kmer_by_base[i][which_adapter];
      if (i < num_bases_counted - 1)
        data << ",";
    }

    data << "]";
    data << ", type : 'line', ";
    data << "name : '" << adapters[which_adapter].first << "'}";
  }
  return data.str();
}

/************** KMER CONTENT ******************************/
ModuleKmerContent::
ModuleKmerContent(const FalcoConfig &config) :
Module("Kmer Content") {
  auto grade_kmer = config.limits.find("kmer")->second;
  grade_warn = grade_kmer.find("warn")->second;
  grade_error = grade_kmer.find("error")->second;
}

void
ModuleKmerContent::summarize_module(const FastqStats &stats) {
  kmer_size = stats.kmer_size;

  // 4^kmer size
  num_kmers = (1 << (2 * kmer_size));
  if (stats.max_read_length < FastqStats::kNumBases) {
    num_kmer_bases = stats.max_read_length;
  } else {
    num_kmer_bases = FastqStats::kNumBases;
  }

  // copies counts of all kmers per base position from stats
  pos_kmer_count = stats.pos_kmer_count;

  // Allocates space for all statistics
  obs_exp_max = vector<double>(num_kmers, 0.0);
  where_obs_exp_is_max = vector<size_t>(num_kmers, 0);
  total_kmer_counts = vector<size_t>(num_kmers, 0);

  // temp variables
  size_t observed_count;
  double expected_count;
  double obs_exp_ratio;
  num_seen_kmers = 0;

  // Here we get the total count of all kmers and the number of observed kmers
  for (size_t kmer = 0; kmer < num_kmers; ++kmer) {
    for (size_t i = kmer_size - 1; i < num_kmer_bases; ++i) {
      observed_count = stats.kmer_count[
                          (i << FastqStats::kBitShiftKmer) | kmer
                       ];
      total_kmer_counts[kmer] += observed_count;
    }
    if (total_kmer_counts[kmer] > 0) ++num_seen_kmers;
  }

  double dividend = static_cast<double>(num_seen_kmers);
  for (size_t kmer = 0; kmer < num_kmers; ++kmer) {
    for (size_t i = kmer_size - 1; i < num_kmer_bases; ++i) {
      observed_count = stats.kmer_count[
                          (i << FastqStats::kBitShiftKmer) | kmer
                          ];
      expected_count = pos_kmer_count[i] / dividend;
      obs_exp_ratio = observed_count / expected_count;

      if (i == 0 || obs_exp_ratio > obs_exp_max[kmer]) {
        obs_exp_max[kmer] = obs_exp_ratio;
        where_obs_exp_is_max[kmer] = i;
      }
    }

    if (obs_exp_max[kmer] > 5) {
      kmers_to_report.push_back(make_pair(kmer, obs_exp_max[kmer]));
    }
  }

  sort (begin(kmers_to_report), end(kmers_to_report),
        [](pair<size_t, double> &a, pair<size_t, double> &b) {
          return a.second > b.second;
        });
}

void
ModuleKmerContent::make_grade() {
  grade = "fail";
}

void
ModuleKmerContent::write_module(ostream &os) {
  os << "#Sequence\tCount\tPValue\tObs/Exp Max\tMax Obs/Exp Position\n";
  for (size_t i = 0; i < 20 && i < kmers_to_report.size(); ++i) {
    size_t kmer = kmers_to_report[i].first;
    os << size_t_to_seq(kmer, kmer_size) << "\t"
       << total_kmer_counts[kmer] << "\t"
       << "0.0" << "\t"
       << obs_exp_max[kmer] << "\t"
       << where_obs_exp_is_max[kmer] << "\n";
  }
}

string
ModuleKmerContent::make_html_data() {
  return "";
}

