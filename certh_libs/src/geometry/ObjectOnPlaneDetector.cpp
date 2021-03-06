#include <certh_libs/ObjectOnPlaneDetector.h>
#include <certh_libs/cvHelpers.h>

#include <pcl/io/pcd_io.h>

#include <pcl/sample_consensus/method_types.h>
#include <pcl/sample_consensus/model_types.h>
#include <pcl/segmentation/sac_segmentation.h>
#include <pcl/octree/octree_impl.h>
#include <pcl/octree/octree_pointcloud_density.h>
#include <pcl/filters/extract_indices.h>
#include <pcl/filters/project_inliers.h>
#include <pcl/common/pca.h>

#include <highgui.h>
#include <ml.h>



#include <boost/filesystem.hpp>
#include <boost/algorithm/string.hpp>


using namespace std ;
using namespace Eigen ;

namespace certh_libs {

ObjectOnPlaneDetector::ObjectOnPlaneDetector(const CloudType &cloud): in_cloud_(cloud)
{

}

ObjectOnPlaneDetector::ObjectOnPlaneDetector(const cv::Mat &depth_im, double fx, double fy, double cx, double cy) {
    in_cloud_ = cvMatToCloud(depth_im, fx, fy, cx, cy) ;
}


CloudType ObjectOnPlaneDetector::cvMatToCloud(const cv::Mat &depth_im, double fx, double fy, double center_x, double center_y)
{
    float constant_x =  0.001/fx ;
    float constant_y =  0.001/fy ;
    float bad_point = std::numeric_limits<float>::quiet_NaN ();

    int w = depth_im.cols, h = depth_im.rows ;

    cv::Mat_<ushort> dim(depth_im) ;

    CloudType cloud(w, h) ;

    for( int i=0 ; i<h ; i++  )
        for(int j=0 ; j<w ; j++ )
        {
            pcl::PointXYZ& pt = cloud.at(j, i) ;
            ushort depth = dim[i][j] ;

            if ( depth == 0 ) {
                pt.x = pt.y = pt.z = bad_point;
            }
            else
            {
                pt.x = (j - center_x) * depth * constant_x;
                pt.y = (i - center_y) * depth * constant_y;
                pt.z = depth/1000.0 ;
            }

        }


    return cloud ;
}

static double calculateCloudDensity(const CloudType &cloud, double voxelSize = 0.01)
{
    CloudType::Ptr in_cloud(new CloudType(cloud)) ;

    pcl::octree::OctreePointCloudDensity<PointType> oc(0.01) ;
    oc.setInputCloud(in_cloud);
    oc.addPointsFromInputCloud ();

    pcl::octree::OctreePointCloudDensity<PointType>::LeafNodeIterator it(oc) ;

    int nVoxels = 0 ;
    double avgDensity = 0.0 ;

    ++it ;
    while ( *it )
    {
        pcl::octree::OctreePointCloudDensityLeaf<int> *leaf = (pcl::octree::OctreePointCloudDensityLeaf<int> *)(*it) ;
        int c = leaf->getPointCounter() ;

        avgDensity += voxelSize*voxelSize*voxelSize/c ;
        nVoxels ++ ;
        ++it ;
    }

    avgDensity /= nVoxels ;

    return pow(avgDensity, 1/3.0) ;

}

bool ObjectOnPlaneDetector::findPlane(Eigen::Vector3d &n, double &d)
{
    int w = in_cloud_.width, h = in_cloud_.height ;

    CloudType::Ptr cloud_in (new CloudType(in_cloud_));

    vector<pcl::ModelCoefficients::Ptr> coefficients ;

    // Segmentation object and configuration

    pcl::SACSegmentation<pcl::PointXYZ> seg;
    seg.setOptimizeCoefficients (true);
    seg.setProbability(0.99);
    seg.setModelType (pcl::SACMODEL_PLANE);
    seg.setMethodType (pcl::SAC_RANSAC);
    seg.setDistanceThreshold (params.fitThreshold);
    seg.setMaxIterations(params.ransacIterations);

    pcl::ExtractIndices<PointType> extract;

    pcl::ModelCoefficients::Ptr coeff (new pcl::ModelCoefficients);
    pcl::PointIndices::Ptr inliers_plane (new pcl::PointIndices);

    // Add input cloud and segment
    seg.setInputCloud (cloud_in);
    seg.segment (*inliers_plane, *coeff);

    if ( inliers_plane->indices.size() == 0 ) return false ;

    n = Eigen::Vector3d(coeff->values[0], coeff->values[1], coeff->values[2]) ;
    d = coeff->values[3] ;

    return true ;
}

cv::Mat ObjectOnPlaneDetector::findObjectMask(const Eigen::Vector3d &n, double d, double thresh, cv::Mat &dmap, vector<cv::Point> &hull)
{
    int w = in_cloud_.width, h = in_cloud_.height ;

    cv::Mat_<uchar> mask = cv::Mat_<uchar>::zeros(h, w) ;
    cv::Mat_<ushort> dmap_ = cv::Mat_<ushort>::zeros(h, w) ;

    for( int i=0 ; i<h ; i++ )
        for(int j=0 ; j<w ; j++ )
        {
            PointType &p = in_cloud_.at(j, i) ;

            if ( !pcl::isFiniteFast(p) ) {
                continue ;
            }

            Eigen::Vector3d p_(p.x, p.y, p.z) ;

            double dist = p_.dot(n) + d ;

            if ( dist < -thresh ) {
                mask[i][j] = 255 ;
                dmap_[i][j] = -dist * 1000 ;
            }
    }


    cv::Mat fgMask = findLargestBlob(mask, hull) ;

    dmap_.copyTo(dmap, fgMask) ;

    return fgMask ;

}


cv::Mat ObjectOnPlaneDetector::refineSegmentation(const cv::Mat &clr, const cv::Mat &fgMask, vector<cv::Point> &hull)
{
    int w = clr.cols, h = clr.rows ;

    cv::Mat result(h, w, CV_8UC1) ;

    for(int i=0 ; i<h ; i++)
        for(int j=0 ; j<w ; j++)
            result.at<uchar>(i, j) = fgMask.at<uchar>(i, j) ? cv::GC_PR_FGD : cv::GC_PR_BGD ;

    cv::Rect rectangle ;

    cv::Mat bgModel,fgModel; // the models (internally used)

    cv::grabCut(clr, result, rectangle, bgModel, fgModel, 1, cv::GC_INIT_WITH_MASK);

    // Get the pixels marked as likely foreground
    cv::compare(result,cv::GC_PR_FGD, result, cv::CMP_EQ);

    cv::Mat fgMask_ = findLargestBlob(result, hull) ;

    // Generate output image
    cv::Mat foreground(clr.size(),CV_8UC3,cv::Scalar(255,255,255));
    clr.copyTo(foreground, fgMask_); // bg pixels not copied

//    cv::imwrite("/tmp/seg.png", result) ;

    return fgMask_ ;
}


}
