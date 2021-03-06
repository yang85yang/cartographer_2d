/*
 * Copyright 2016 The Cartographer Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "cartographer/mapping_2d/submaps.h"

#include <cinttypes>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <limits>

#include "Eigen/Geometry"
#include "cartographer/common/make_unique.h"
#include "cartographer/common/port.h"
#include "glog/logging.h"

namespace cartographer {
namespace mapping_2d {

ProbabilityGrid ComputeCroppedProbabilityGrid(const ProbabilityGrid& probability_grid) 
{
	Eigen::Array2i offset;
	CellLimits limits;
	probability_grid.ComputeCroppedLimits(&offset, &limits);
	
	const double resolution = probability_grid.limits().resolution();
	const Eigen::Vector2d max = probability_grid.limits().max() - resolution * Eigen::Vector2d(offset.y(), offset.x());
	ProbabilityGrid cropped_grid(MapLimits(resolution, max, limits));
	for ( const Eigen::Array2i& xy_index : XYIndexRangeIterator(limits) ) 
	{
		if (probability_grid.IsKnown(xy_index + offset))
			cropped_grid.SetProbability(xy_index, probability_grid.GetProbability(xy_index + offset));
	}
	
	return cropped_grid;
}

proto::SubmapsOptions CreateSubmapsOptions(
    common::LuaParameterDictionary* const parameter_dictionary) {
  proto::SubmapsOptions options;
  options.set_resolution(parameter_dictionary->GetDouble("resolution"));
  options.set_num_range_data(
      parameter_dictionary->GetNonNegativeInt("num_range_data"));
  *options.mutable_range_data_inserter_options() =
      CreateRangeDataInserterOptions(
          parameter_dictionary->GetDictionary("range_data_inserter").get());
  CHECK_GT(options.num_range_data(), 0);
  return options;
}

Submap::Submap(const MapLimits& limits, const Eigen::Vector2f& origin)
    : mapping::Submap(transform::Rigid3d::Translation(
          Eigen::Vector3d(origin.x(), origin.y(), 0.))),
      probability_grid_(limits) {}

// 这个构造函数可能是在载入地图时用来构造submap的，而不是新建地图时用的
Submap::Submap(const mapping::proto::Submap2D& proto)
    : mapping::Submap(transform::ToRigid3(proto.local_pose())),
      probability_grid_(ProbabilityGrid(proto.probability_grid())) {
  SetNumRangeData(proto.num_range_data());
  finished_ = proto.finished();
}

void Submap::ToProto(mapping::proto::Submap* const proto) const {
  auto* const submap_2d = proto->mutable_submap_2d();
  *submap_2d->mutable_local_pose() = transform::ToProto(local_pose());
  submap_2d->set_num_range_data(num_range_data());
  submap_2d->set_finished(finished_);
  *submap_2d->mutable_probability_grid() = probability_grid_.ToProto();
}

void Submap::ToResponseProto(
    const transform::Rigid3d&,
    mapping::proto::SubmapQuery::Response* const response) const {
  response->set_submap_version(num_range_data());

  Eigen::Array2i offset;
  CellLimits limits;
  probability_grid_.ComputeCroppedLimits(&offset, &limits);

  std::string cells;
  for (const Eigen::Array2i& xy_index : XYIndexRangeIterator(limits)) {
    if (probability_grid_.IsKnown(xy_index + offset)) {
      // We would like to add 'delta' but this is not possible using a value and
      // alpha. We use premultiplied alpha, so when 'delta' is positive we can
      // add it by setting 'alpha' to zero. If it is negative, we set 'value' to
      // zero, and use 'alpha' to subtract. This is only correct when the pixel
      // is currently white, so walls will look too gray. This should be hard to
      // detect visually for the user, though.
      const int delta =
          128 - mapping::ProbabilityToLogOddsInteger(
                    probability_grid_.GetProbability(xy_index + offset));
      const uint8 alpha = delta > 0 ? 0 : -delta;
      const uint8 value = delta > 0 ? delta : 0;
      cells.push_back(value);
      cells.push_back((value || alpha) ? alpha : 1);
    } else {
      constexpr uint8 kUnknownLogOdds = 0;
      cells.push_back(static_cast<uint8>(kUnknownLogOdds));  // value
      cells.push_back(0);                                    // alpha
    }
  }
  mapping::proto::SubmapQuery::Response::SubmapTexture* const texture =
      response->add_textures();
  common::FastGzipString(cells, texture->mutable_cells());

  texture->set_width(limits.num_x_cells);
  texture->set_height(limits.num_y_cells);
  const double resolution = probability_grid_.limits().resolution();
  texture->set_resolution(resolution);
  const double max_x =
      probability_grid_.limits().max().x() - resolution * offset.y();
  const double max_y =
      probability_grid_.limits().max().y() - resolution * offset.x();
  *texture->mutable_slice_pose() = transform::ToProto(
      local_pose().inverse() *
      transform::Rigid3d::Translation(Eigen::Vector3d(max_x, max_y, 0.)));
}

void Submap::InsertRangeData(const sensor::RangeData& range_data,
                             const RangeDataInserter& range_data_inserter) 
{
	CHECK(!finished_);
	range_data_inserter.Insert(range_data, &probability_grid_);
	SetNumRangeData(num_range_data() + 1);
}

void Submap::Finish() {
  CHECK(!finished_);
  probability_grid_ = ComputeCroppedProbabilityGrid(probability_grid_);
  finished_ = true;
}

ActiveSubmaps::ActiveSubmaps(const proto::SubmapsOptions& options)
    : options_(options),
      range_data_inserter_(options.range_data_inserter_options()) 
{
	// We always want to have at least one likelihood field which we can return,
	// and will create it at the origin in absence of a better choice.
	// 构造的时候就添加一个submap，虽然可能没有数据
	AddSubmap(Eigen::Vector2f::Zero());
}

void ActiveSubmaps::InsertRangeData(const sensor::RangeData& range_data) 
{
	// 初始化时，vector里会有一个新建好的 submap，这是insert range data会针对这个submap进行操作（插入传入的rangedata）
	// 当最新的这个 submap 的数据已经全部添加完毕（也就是num_range_data超过配置的值），就会添加新的submap
	// 这时传入的新的rangedata就会插入到vector中的两个 submap 中（这个 vector 中的 submap 数量最多为 2 个）
	// TODO(edward) 为什么要插入到前后两个submap中？？
	// 所以每个 submap 中其实插入了 2 × num_range_data 个 range data，这个在 test 里也可以看到
	// 这个可否做优化，优化成如下的只插入到当前这个 submap 中，也就是 submaps_.back() 里
	// 	submaps_.back()->InsertRangeData( range_data, range_data_inserter_ );
	
	for ( auto& submap : submaps_ ) 
		submap->InsertRangeData( range_data, range_data_inserter_ );
	
	if ( submaps_.back()->num_range_data() == options_.num_range_data() ) 
		AddSubmap(range_data.origin.head<2>());
}

void ActiveSubmaps::FinishSubmap() 
{
	Submap* submap = submaps_.front().get();
	submap->Finish();
	++matching_submap_index_;
	submaps_.erase(submaps_.begin());
}

// ActiveSubmaps 里的这个 submap 的 vector 最多只维护这两个 submap 的数据
// 而且只要数量超过 1 个的时候，添加下一个submap之前会先结束掉之前的submap
void ActiveSubmaps::AddSubmap(const Eigen::Vector2f& origin) 
{
	if (submaps_.size() > 1) 
	{
		// This will crop the finished Submap before inserting a new Submap to
		// reduce peak memory usage a bit.
		FinishSubmap();
	}
	constexpr int kInitialSubmapSize = 100;
	submaps_.push_back(common::make_unique<Submap>(
		MapLimits(options_.resolution(),
					origin.cast<double>() + 0.5 * kInitialSubmapSize *
												options_.resolution() *
												Eigen::Vector2d::Ones(),
					CellLimits(kInitialSubmapSize, kInitialSubmapSize)),
		origin));
	LOG(INFO) << "Added submap " << matching_submap_index_ + submaps_.size();
}

}  // namespace mapping_2d
}  // namespace cartographer
