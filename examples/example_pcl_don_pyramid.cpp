/**
 * @file pcl_don.cpp
 * Difference of normals implementation using PCL.
 *
 * @author Yani Ioannou
 * @date 2012-03-11
 */

#include <boost/program_options.hpp>
#include <boost/accumulators/accumulators.hpp>
#include <boost/accumulators/statistics/stats.hpp>

// Headers specifics to the computations we need
#include <boost/accumulators/statistics/median.hpp>
#include <boost/accumulators/statistics/mean.hpp>
#include <boost/accumulators/statistics/min.hpp>
#include <boost/accumulators/statistics/max.hpp>
#include <boost/accumulators/statistics/sum.hpp>
#include <boost/accumulators/statistics/count.hpp>
#include <boost/accumulators/statistics/variance.hpp>
#include <string>
#include <cmath>
#include <map>

#include <pcl/point_types.h>
#include <pcl/io/pcd_io.h>
#include <pcl/kdtree/kdtree_flann.h>
#include <pcl/common/point_operators.h>
#include <pcl/common/io.h>
#include <pcl/search/organized.h>
#include <pcl/search/octree.h>
#include <pcl/search/kdtree.h>
#include <pcl/features/normal_3d_omp.h>
#include <pcl/filters/conditional_removal.h>
#include <pcl/segmentation/sac_segmentation.h>
#include <pcl/segmentation/extract_clusters.h>
#include <pcl/surface/gp3.h>
#include <pcl/io/vtk_io.h>
#include <pcl/filters/voxel_grid.h>

#include <pcl/features/don.h>

using namespace pcl;
using namespace boost::accumulators;
using namespace std;

namespace po = boost::program_options;

typedef pcl::PointXYZ PointT;
typedef pcl::PointNormal PointNT;
typedef pcl::PointNormal PointOutT;
typedef pcl::search::Search<PointT>::Ptr SearchPtr;

typedef accumulator_set<double, stats<tag::median(with_p_square_quantile), tag::mean, tag::min, tag::max, tag::count, tag::sum, tag::variance(lazy) > > accumulator_t;

int main(int argc, char *argv[])
{
  ///The smallest radius to use in the DoN filter.
  double radius1;

  ///The largest radius to use in the DoN filter.
  double radius2;

  ///The radius increment to use
  double radiusincrement;

  ///The file to read the model from.
  vector<string> modelfiles;

  double decimation;

  // Declare the supported options.
  po::options_description desc("Program options");
  desc.add_options()
          //Program mode option
          ("help", "produce help message")
          //Options
          ("smallradius", po::value<double>(&radius1)->required(), "the smallest radius to use in the DoN filter")
          ("largeradius", po::value<double>(&radius2)->required(), "the largest radius to use in the DoN filter")
          ("radiusincrement", po::value<double>(&radiusincrement)->required(), "the largest radius to use in the DoN filter")
          ("modelfiles", po::value<vector<string> >(&modelfiles)->required(), "the files to read a point cloud model from")
          ("approx", po::value<double>(&decimation), "voxelization factor of pointcloud to use in approximation of normals")
          ;

  po::positional_options_description p;
  p.add("modelfiles", -1);

  po::variables_map vm;
  po::store(po::command_line_parser(argc, argv).
            options(desc).positional(p).run(), vm);

  // Print help
  if (vm.count("help"))
  {
          cout << desc << "\n";
          return false;
  }

  // Process options.
  po::notify(vm);

  // Load cloud in blob format
  sensor_msgs::PointCloud2 blob;

  //cumulative stats for all models
  map< pair<float, float>, boost::shared_ptr<accumulator_t> > stats;

  for(vector<string>::iterator modelfile = modelfiles.begin(); modelfile != modelfiles.end(); modelfile++ )
  {
    pcl::io::loadPCDFile (modelfile->c_str(), blob);

    pcl::PointCloud<PointT>::Ptr cloud (new pcl::PointCloud<PointT>);
    cout << "#Loading model point cloud " << modelfile->c_str();
    pcl::fromROSMsg (blob, *cloud);
    cout << "#done." << endl;

    SearchPtr tree;

    if (cloud->isOrganized ())
    {
      tree.reset (new pcl::search::OrganizedNeighbor<PointT> ());
    }
    else
    {
      /**
       * NOTE: Some PCL versions contain a KDTree with a critical bug in
       * which setSearchRadius is ineffective (always uses K neighbours).
       *
       * Since DoN *requires* a fixed search radius, if you are getting
       * strange results in unorganized data, compare them with that
       * while using the Octree search method.
       */
      //tree.reset (new pcl::search::Octree<PointT> (scale1/10));
      tree.reset (new pcl::search::KdTree<PointT> (false));
    }
    tree->setInputCloud (cloud);

    PointCloud<PointT>::Ptr small_cloud_downsampled;
    PointCloud<PointT>::Ptr large_cloud_downsampled;
    for(float scale2 = radius1+radiusincrement; scale2 < radius2; scale2 += radiusincrement){
      for(float scale1 = radius1; scale1 < scale2; scale1+= radiusincrement){
        // If we are using approximation
        if(vm.count("approx")){
          cout << "#Downsampling point cloud for approximation" << endl;

          // Create the downsampling filtering object
          pcl::VoxelGrid<PointT> sor;
          sor.setDownsampleAllData (false);
          sor.setInputCloud (cloud);

          // Create downsampled point cloud for DoN NN search with small scale
          small_cloud_downsampled = PointCloud<PointT>::Ptr(new pcl::PointCloud<PointT>);
          const float smalldownsample = scale1/decimation;
          sor.setLeafSize (smalldownsample, smalldownsample, smalldownsample);
          sor.filter (*small_cloud_downsampled);
          cout << "#Using leaf size of " << smalldownsample << " for small scale, " << small_cloud_downsampled->size() << " points" << endl;

          // Create downsampled point cloud for DoN NN search with large scale
          large_cloud_downsampled = PointCloud<PointT>::Ptr(new pcl::PointCloud<PointT>);
          const float largedownsample = scale2/decimation;
          sor.setLeafSize (largedownsample, largedownsample, largedownsample);
          sor.filter (*large_cloud_downsampled);
          cout << "#Using leaf size of " << largedownsample << " for large scale, " << large_cloud_downsampled->size() << " points" << endl;
        }

        // Compute normals using both small and large scales at each point
        pcl::NormalEstimationOMP<PointT, PointNT> ne;
        ne.setInputCloud (cloud);
        ne.setSearchMethod (tree);

        /**
         * NOTE: setting viewpoint is very important, so that we can ensure
         * normals are all pointed in the same direction!
         */
        ne.setViewPoint(std::numeric_limits<float>::max(),std::numeric_limits<float>::max(),std::numeric_limits<float>::max());

        if(scale1 >= scale2){
          cerr << "Error: Large scale must be > small scale!" << endl;
          exit(EXIT_FAILURE);
        }

        //the normals calculated with the small scale
        //cout << "Calculating normals for scale..." << scale1 << endl;
        pcl::PointCloud<PointNT>::Ptr normals_small_scale (new pcl::PointCloud<PointNT>);

        if(vm.count("approx")){
          ne.setSearchSurface(small_cloud_downsampled);
        }
        ne.setRadiusSearch (scale1);
        ne.compute (*normals_small_scale);

        //cout << "Calculating normals for scale..." << scale2 << endl;
        //the normals calculated with the large scale
        pcl::PointCloud<PointNT>::Ptr normals_large_scale (new pcl::PointCloud<PointNT>);

        if(vm.count("approx")){
          ne.setSearchSurface(large_cloud_downsampled);
        }
        ne.setRadiusSearch (scale2);
        ne.compute (*normals_large_scale);

        // Create output cloud for DoN results
        PointCloud<PointOutT>::Ptr doncloud (new pcl::PointCloud<PointOutT>);
        copyPointCloud<PointT, PointOutT>(*cloud, *doncloud);

        //cout << "Calculating DoN... " << endl;
        // Create DoN operator
        pcl::DifferenceOfNormalsEstimation<PointT, PointNT, PointOutT> don;
        don.setInputCloud (cloud);
        don.setNormalScaleLarge(normals_large_scale);
        don.setNormalScaleSmall(normals_small_scale);

        if(!don.initCompute ()){
          std::cerr << "Error: Could not intialize DoN feature operator" << std::endl;
          exit(EXIT_FAILURE);
        }

        //Compute DoN
        don.computeFeature(*doncloud);

        //Find statistics for the given model
        //mean, median, maximum, minimum and stddev
        //accumulator_set<Eigen::Vector3f, stats<tag::mean> > acc_vector(Eigen::Vector3f::Zero());
        //accumulator_set<double, stats<tag::median(with_p_square_quantile), tag::mean, tag::min, tag::max, tag::count, tag::sum, tag::variance(lazy) > > acc_mag(0);

        //perform DoN subtraction and return results
        for (size_t point_id = 0; point_id < doncloud->points.size (); ++point_id)
        {
          Eigen::Vector3f don = doncloud->points[point_id].getNormalVector3fMap();
          //acc_vector(don);
          float mag = don.norm();
          pair<float, float> scalekey = make_pair(scale1, scale2);
          if(stats.find(scalekey) != stats.end()){
            (*(stats.at(scalekey)))(mag);
          }else{
            stats.insert(make_pair(scalekey, new accumulator_t(0)));
            (*(stats.at(scalekey)))(mag);
          }
        }
      }
    }
  }

  std::cout << "#r_s, r_l, min, max, count, sum, median, mean, variance" << std::endl;

  for(map< pair<float, float>, boost::shared_ptr<accumulator_t> >::iterator i = stats.begin(); i != stats.end(); i++){
    std::cout << i->first.first << ", "
        << i->first.second << ", "
        << boost::accumulators::extract::min(*i->second) << ", "
        << boost::accumulators::extract::max(*i->second) << ", "
        << boost::accumulators::extract::count(*i->second) << ", "
        << sum(*i->second) << ", "
        << median(*i->second) << ", "
        << mean(*i->second) << ", "
        << variance(*i->second) << std::endl;
    //for missing data
    std::cout << i->first.second << ", "
            << i->first.first << ", "
            << 0 << ", "
            << 0 << ", "
            << 0 << ", "
            << 0 << ", "
            << 0 << ", "
            << 0 << ", "
            << 0 << std::endl;
  }

  return (0);
}