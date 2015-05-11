/*---------------------------------------------------------------------------*
 |                         Object Labeling Toolkit                           |
 |            A set of software components for the management and            |
 |                      labeling of RGB-D datasets                           |
 |                                                                           |
 |              Copyright (C) 2015 Jose Raul Ruiz Sarmiento                  |
 |                 University of Malaga <jotaraul@uma.es>                    |
 |             MAPIR Group: <http://http://mapir.isa.uma.es/>                |
 |                                                                           |
 |   This program is free software: you can redistribute it and/or modify    |
 |   it under the terms of the GNU General Public License as published by    |
 |   the Free Software Foundation, either version 3 of the License, or       |
 |   (at your option) any later version.                                     |
 |                                                                           |
 |   This program is distributed in the hope that it will be useful,         |
 |   but WITHOUT ANY WARRANTY; without even the implied warranty of          |
 |   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the            |
 |   GNU General Public License for more details.                            |
 |   <http://www.gnu.org/licenses/>                                          |
 |                                                                           |
 *---------------------------------------------------------------------------*/

#include <mrpt/maps/CSimplePointsMap.h>
#include <mrpt/obs/CObservation2DRangeScan.h>
#include <mrpt/obs/CObservation3DRangeScan.h>
#include <mrpt/slam/CICP.h>
#include <mrpt/obs/CRawlog.h>
#include <mrpt/poses/CPose2D.h>
#include <mrpt/poses/CPosePDF.h>
#include <mrpt/poses/CPosePDFGaussian.h>
#include <mrpt/gui.h>
#include <mrpt/math/utils.h>
#include <mrpt/system/threads.h>
#include <mrpt/utils/CTicTac.h>
#include <mrpt/maps/PCL_adapters.h>

#include <mrpt/slam/CICP.h>

// ICP 3D MRPT

#include <mrpt/poses/CPose3DPDF.h>
#include <mrpt/system/threads.h>
#include <mrpt/gui/CDisplayWindow3D.h>
#include <mrpt/opengl/CGridPlaneXY.h>
#include <mrpt/opengl/CSphere.h>
#include <mrpt/opengl/CAngularObservationMesh.h>
#include <mrpt/opengl/CDisk.h>
#include <mrpt/opengl/stock_objects.h>

// ICP 3D PCL

#include <pcl/point_types.h>
#include <pcl/point_cloud.h>
#include <pcl/registration/gicp.h>
#include <pcl/registration/warp_point_rigid_3d.h>

//#include <pcl/filters/voxel_grid.h>
#include <pcl/filters/fast_bilateral.h>

#include <pcl/visualization/pcl_visualizer.h>

#include <pcl/visualization/cloud_viewer.h>

#include <pcl/surface/convex_hull.h>
#include <pcl/filters/crop_hull.h>

#include <iostream>
#include <fstream>

#include <numeric> // std::accumulate

using namespace mrpt;
using namespace mrpt::utils;
using namespace mrpt::maps;
using namespace mrpt::slam;
using namespace mrpt::gui;
using namespace mrpt::opengl;
using namespace mrpt::poses;
using namespace mrpt::obs;
using namespace mrpt::system;
using namespace std;
using namespace mrpt::math;

using namespace pcl;

//
// Configuration
//

bool initialGuessICP2D  = true;
bool refineWithICP3D    = false;
bool accumulatePast     = false;
bool useKeyPoses        = false;
bool smooth3DObs        = false;
bool useOverlappingObs  = false;
bool visualize2DResults = true;

string ICP3D_method;

CPose3D lastGoodICP3Dpose;
bool oneGoodICP3DPose = false;

bool skip_window=false;
//int  ICP_method = (int) icpClassic;
int  ICP_method = (int) icpLevenbergMarquardt;

CPose2D		initialPose(0.8f,0.0f,(float)DEG2RAD(0.0f));
gui::CDisplayWindowPlots	win("ICP results");

ofstream trajectoryFile("trajectory.txt",ios::trunc);

CDisplayWindow3D window("ICP-3D demo: scene",500,500);
CDisplayWindow3D window2("ICP-3D demo: UNALIGNED scans",500,500);
CDisplayWindow3D window3("ICP-3D demo: ICP-ALIGNED scans",500,500);

//Increase this values to get more precision. It will also increase run time.
const size_t HOW_MANY_YAWS=360;
const size_t HOW_MANY_PITCHS=360;

const size_t SECS_PER_MIN = 60;
const size_t SECS_PER_HOUR = 3600;

const double KEY_POSE_DIST_THRESHOLD = 0.3;
const double KEY_POSE_ANGLE_THRESHOLD = DEG2RAD((float)20);

struct TRobotPose
{
    TTimeStamp  time;
    CPose2D     pose;
    double      goodness;
};

struct T3DRangeScan
{
    CObservation3DRangeScanPtr obs;
    pcl::PointCloud<pcl::PointXYZ>::Ptr convexHullCloud;
    vector<pcl::Vertices> polygons;

    T3DRangeScan() : obs(new CObservation3DRangeScan()),
        convexHullCloud(new pcl::PointCloud<pcl::PointXYZ>()) {}
};

vector< TRobotPose > v_robotPoses;
vector< T3DRangeScan > v_3DRangeScans;
vector< CObservation3DRangeScanPtr > v_pending3DRangeScans;
vector< double > v_goodness;


//-----------------------------------------------------------
//                      smoothing
//-----------------------------------------------------------

void smoothObs( CObservation3DRangeScanPtr obsToSmooth )
{
    CObservation3DRangeScanPtr obs3D = CObservation3DRangeScan::Create();
    obs3D = obsToSmooth;

    // Remove the sensor pose

    obs3D.make_unique();
    obs3D->sensorPose.setFromValues(0,0,0,0,0,0);

    pcl::PointCloud<pcl::PointXYZ>::Ptr pcl_cloud( new pcl::PointCloud<pcl::PointXYZ>() );
    obs3D->project3DPointsFromDepthImageInto( *pcl_cloud, true );

    pcl_cloud->height = 240;
    pcl_cloud->width = 320;

    // Apply bilateral filter

    pcl::FastBilateralFilter<pcl::PointXYZ> m_bilateralFilter;

    m_bilateralFilter.setInputCloud (pcl_cloud);

    m_bilateralFilter.setSigmaS (10);
    m_bilateralFilter.setSigmaR (0.05);
    //m_bilateralFilter.setEarlyDivision (m_bilateralFilterConf.earlyDivision);

    m_bilateralFilter.filter (*pcl_cloud);

    // Fill the original obs

    obs3D->points3D_x.clear();
    obs3D->points3D_y.clear();
    obs3D->points3D_z.clear();

    size_t N_points = pcl_cloud->points.size();
    obs3D->points3D_x.resize(N_points);
    obs3D->points3D_y.resize(N_points);
    obs3D->points3D_z.resize(N_points);

    for ( size_t point_index = 0;
          point_index < N_points;
          point_index++ )
    {
        obsToSmooth->points3D_x[point_index] = pcl_cloud->points[point_index].x;
        obsToSmooth->points3D_y[point_index] = pcl_cloud->points[point_index].y;
        obsToSmooth->points3D_z[point_index] = pcl_cloud->points[point_index].z;
    }
}

//-----------------------------------------------------------
//                      isKeyPose
//-----------------------------------------------------------

bool isKeyPose( CPose3D &pose, CPose3D &lastPose)
{
    CVectorDouble pose_v;
    pose.getAsVector( pose_v );

    CVectorDouble lastPose_v;
    lastPose.getAsVector( lastPose_v );

    //    cout << "Last pose    : " << lastPose << endl;
    //    cout << "Current pose : " << pose << endl;

    float dist = sqrt(pow(pose_v[0]-lastPose_v[0],2) +
                      pow(pose_v[1]-lastPose_v[1],2) ); // x-y distance
    float angle = abs( pose_v[3]-lastPose_v[3] ); // Diff in yaw

    //    cout << "Distance: " << dist << " and angle: " << RAD2DEG(angle) << endl;

    if ( ( dist > KEY_POSE_DIST_THRESHOLD )
         || ( angle > KEY_POSE_ANGLE_THRESHOLD ) )
        return true;
    else
        return false;
}

//-----------------------------------------------------------
//                      trajectoryICP2D
//-----------------------------------------------------------

void trajectoryICP2D( string &simpleMapFile, CRawlog &rawlog,
                      CObservation2DRangeScanPtr obs2D, double &goodness )
{
    CSimplePointsMap		m1,m2;
    float					runningTime;
    CICP::TReturnInfo		info;
    CICP					ICP;

    m1.load2D_from_text_file(simpleMapFile);
    //cout << "M1 size: " << m1.size();

    m2.insertObservation( obs2D.pointer() );
    //cout << "M2 size: " << m2.size();

    // -----------------------------------------------------
    //	ICP.options.ICP_algorithm = icpLevenbergMarquardt;
    //	ICP.options.ICP_algorithm = icpClassic;
    ICP.options.ICP_algorithm = (TICPAlgorithm)ICP_method;

    ICP.options.maxIterations			= 800;
    ICP.options.thresholdAng			= DEG2RAD(10.0f);
    ICP.options.thresholdDist			= 0.75f;
    ICP.options.ALFA					= 0.99f;
    ICP.options.smallestThresholdDist	= 0.05f;
    ICP.options.doRANSAC = false;

    //    ICP.options.dumpToConsole();
    // -----------------------------------------------------

    CPosePDFPtr pdf = ICP.Align(
                &m1,
                &m2,
                initialPose,
                &runningTime,
                (void*)&info);

    printf("ICP run in %.02fms, %d iterations (%.02fms/iter), %.01f%% goodness\n -> ",
           runningTime*1000,
           info.nIterations,
           runningTime*1000.0f/info.nIterations,
           info.goodness*100 );

    goodness = info.goodness*100;

    cout << "Mean of estimation: " << pdf->getMeanVal() << endl<< endl;
    initialPose = pdf->getMeanVal();

    CPosePDFGaussian  gPdf;
    gPdf.copyFrom(*pdf);

    //    cout << "Covariance of estimation: " << endl << gPdf.cov << endl;

    //    cout << " std(x): " << sqrt( gPdf.cov(0,0) ) << endl;
    //    cout << " std(y): " << sqrt( gPdf.cov(1,1) ) << endl;
    //    cout << " std(phi): " << RAD2DEG(sqrt( gPdf.cov(2,2) )) << " (deg)" << endl;

    //    cout << "-> Saving reference map as scan1.txt" << endl;
    //    m1.save2D_to_text_file("scan1.txt");

    //    cout << "-> Saving map to align as scan2.txt" << endl;
    //    m2.save2D_to_text_file("scan2.txt");

    //    cout << "-> Saving transformed map to align as scan2_trans.txt" << endl;
    CSimplePointsMap m2_trans = m2;
    m2_trans.changeCoordinatesReference( gPdf.mean );
    //    m2_trans.save2D_to_text_file("scan2_trans.txt");


    trajectoryFile << initialPose << endl;


    if (!skip_window)
    {
        CMatrixFloat COV22 =  CMatrixFloat( CMatrixDouble( gPdf.cov ));
        COV22.setSize(2,2);
        Eigen::Vector2f MEAN2D(2);
        MEAN2D(0) = gPdf.mean.x();
        MEAN2D(1) = gPdf.mean.y();

        // Reference map:
        vector<float>   map1_xs, map1_ys, map1_zs;
        m1.getAllPoints(map1_xs,map1_ys,map1_zs);
        win.plot( map1_xs, map1_ys, "b.3", "map1");

        // Translated map:
        vector<float>   map2_xs, map2_ys, map2_zs;
        m2_trans.getAllPoints(map2_xs,map2_ys,map2_zs);
        win.plot( map2_xs, map2_ys, "r.3", "map2");

        // Uncertainty
        win.plotEllipse(MEAN2D(0),MEAN2D(1),COV22,3.0,"b2", "cov");

        win.axis(-1,10,-6,6);
        win.axis_equal();

        /*cout << "Close the window to exit" << endl;
        win.waitForKey();*/
        mrpt::system::sleep(0);
    }

}


//-----------------------------------------------------------
//                 processPending3DRangeScans
//-----------------------------------------------------------

void processPending3DRangeScans()
{
    size_t N_robotPoses = v_robotPoses.size();

    // More than one hokuyo observation?
    if ( N_robotPoses > 1 )
    {
        TRobotPose &rp1 = v_robotPoses[N_robotPoses-2];
        TRobotPose &rp2 = v_robotPoses[N_robotPoses-1];

        double tdPositions = timeDifference(rp1.time,rp2.time);

        // Okey, set pose of all the pending scans
        for ( size_t obs_index = 0;
              obs_index < v_pending3DRangeScans.size();
              obs_index++ )
        {
            CPose3D pose;
            v_pending3DRangeScans[obs_index]->getSensorPose(pose);

            TTimeStamp &obsTime = v_pending3DRangeScans[obs_index]->timestamp;

            double tdPos1Obs = timeDifference(rp1.time,obsTime);

            // Get an approximation of where was gathered the 3D range scan
            double interpolationFactor = ( tdPos1Obs / tdPositions );

            CPose2D posPoseDif = rp2.pose - rp1.pose;

            CVectorDouble v_coords;
            posPoseDif.getAsVector(v_coords);
            CPose2D intermediatePose(v_coords[0]*interpolationFactor,
                                     v_coords[1]*interpolationFactor,
                                     v_coords[2]*interpolationFactor);

            CVectorDouble v_coordsIntermediatePose;
            intermediatePose.getAsVector( v_coordsIntermediatePose );

            CVectorDouble v_rp1Pose;
            rp1.pose.getAsVector( v_rp1Pose );

            CPose2D poseToSum = rp1.pose + intermediatePose;

            CPose3D finalPose = poseToSum + pose;

            CVectorDouble coords;
            finalPose.getAsVector(coords);
            //finalPose.setFromValues(coords[0],coords[1],coords[2],coords[3],coords[4]-DEG2RAD(90),coords[5]-DEG2RAD(90));
            finalPose.setFromValues(coords[0],coords[1],coords[2],coords[3],coords[4],coords[5]);

            /*cout << "Obs initial pose:       " << pose << endl;
            cout << "interpolationFactor:    " << interpolationFactor << endl;
            cout << "rp1.pose:               " << rp1.pose << endl;
            cout << "rp2.pose:               " << rp2.pose << endl;
            cout << "intermediatePose:       " << intermediatePose << endl;
            cout << "poseToSum:              " << poseToSum << endl;
            cout << "finalPose:              " << finalPose << endl;*/

            v_pending3DRangeScans[obs_index]->setSensorPose( finalPose );

            T3DRangeScan Obs3D;
            Obs3D.obs = v_pending3DRangeScans[obs_index];
            v_3DRangeScans.push_back( Obs3D );
        }
    }

    v_pending3DRangeScans.clear();

}


//-----------------------------------------------------------
//                      refineLocationPCL
//-----------------------------------------------------------

void refineLocationGICP3D( vector<T3DRangeScan> &v_obs, vector<T3DRangeScan> &v_obs2 )
{
    if (!initialGuessICP2D)
    {
        for (size_t i=0; i < v_obs2.size(); i++)
        {
            CPose3D pose;
            v_obs[v_obs.size()-v_obs2.size()+i].obs->getSensorPose(pose);
            v_obs2[i].obs->setSensorPose(pose);
        }
    }

    // Show the scanned points:
    CSimplePointsMap	M1,M2;
    //M1.insertionOptions.minDistBetweenLaserPoints = 0.01;
    //M2.insertionOptions.minDistBetweenLaserPoints = 0.01;

    CTicTac clockInsertionTime;
    clockInsertionTime.Tic();

    // Insert observations into a points map

    for ( size_t i = 0; i < v_obs.size(); i++ )
    {

        if ( !useOverlappingObs )
        {
            M1.insertObservationPtr( v_obs[i].obs );
        }
        else
        {
            // Now check if this convex hull overlaps with the convex hull of the
            // new observations

            size_t j = 0;
            bool insert = false;
            while ( !insert && ( j < v_obs2.size() ) )
            {
                // Check overlapping
                pcl::PointCloud<pcl::PointXYZ>::Ptr outputCloud(new pcl::PointCloud<pcl::PointXYZ>());

                // Point cloud new with old
                pcl::CropHull<pcl::PointXYZ> cropHull;
                cropHull.setInputCloud( v_obs2[j].convexHullCloud );
                cropHull.setHullIndices( v_obs[i].polygons);
                cropHull.setHullCloud( v_obs[i].convexHullCloud);
                cropHull.setDim(3);

                std::vector<int> indices;
                cropHull.filter(indices);
                cropHull.filter(*outputCloud);

                if ( !outputCloud->size() ) // Exists overlap?
                {
                    // Point cloud old with new. This is needed becouse we are
                    // checking only a few points on the point clouds boundaries
                    cropHull.setInputCloud( v_obs[i].convexHullCloud );
                    cropHull.setHullIndices( v_obs2[j].polygons );
                    cropHull.setHullCloud( v_obs2[j].convexHullCloud );
                    cropHull.setDim(3);

                    cropHull.filter(indices);
                    cropHull.filter(*outputCloud);

                    if ( outputCloud->size() )
                        insert = true;
                }
                else
                    insert = true;

                j++;
            }

            if ( insert )
                M1.insertObservationPtr( v_obs[i].obs );
        }

    }

    for ( size_t i = 0; i < v_obs2.size(); i++ )
        M2.insertObservationPtr( v_obs2[i].obs );

    cout << "Time spent inserting points: " << clockInsertionTime.Tac() << " s." << endl;

    cout << "Getting points... points 1: ";

    CVectorDouble xs, ys, zs, xs2, ys2, zs2;
    M1.getAllPoints(xs,ys,zs);
    M2.getAllPoints(xs2,ys2,zs2);

    cout << xs.size() << " points 2: " << xs2.size() << " ... done" << endl;

    //cout << "Inserting points...";

    PointCloud<PointXYZ>::Ptr	cloud_old   (new PointCloud<PointXYZ>());
    PointCloud<PointXYZ>::Ptr	cloud_new   (new PointCloud<PointXYZ>());
    PointCloud<PointXYZ>::Ptr   cloud_trans (new PointCloud<PointXYZ>());

    cloud_old->clear();
    cloud_new->clear();
    cloud_trans->clear();

    for ( size_t i = 0; i < xs.size(); i+= ( ( accumulatePast ) ? 2 : 1) )
        cloud_old->push_back(PointXYZ(xs[i],ys[i],zs[i]));

    for ( size_t i = 0; i < xs2.size(); i+= ( ( accumulatePast ) ? 1 : 1) )
        cloud_new->push_back(PointXYZ(xs2[i],ys2[i],zs2[i]));

    // Check if the cloud has points (GICP crashes if so)
    if ( cloud_new->points.size() < 100 )
        return;

    GeneralizedIterativeClosestPoint<PointXYZ, PointXYZ> gicp;

    //cout << "Setting input clouds...";

    gicp.setInputSource(cloud_new);
    gicp.setInputTarget(cloud_old);

    //cout << "done"  << endl;
    //cout << "Setting parameters...";

    //ICP options
    gicp.setMaxCorrespondenceDistance (0.2); // 0.5
    // Set the maximum number of iterations (criterion 1)
    gicp.setMaximumIterations (20); // 10
    // Set the transformation tras epsilon (criterion 2)
    gicp.setTransformationEpsilon (1e-5); // 1e-5
    // Set the transformation rot epsilon (criterion 3)
    gicp.setRotationEpsilon (1e-5); // 1e-5

    //cout << "done"  << endl;

    cout << "Doing ICP...";

    gicp.align(*cloud_trans);

    double score;
    score = gicp.getFitnessScore(); // Returns the squared average error between the aligned input and target

    v_goodness.push_back(score);

    cout << " done! Average error: " << sqrt(score) << " meters" << endl;

//    if ( score > 0.1 )
//        return;

    // Obtain the transformation that aligned cloud_source to cloud_source_registered
    Eigen::Matrix4f transformation = gicp.getFinalTransformation();

    CPose3D estimated_pose;

    CMatrixDouble33 rot_matrix;
    for (unsigned int i=0; i<3; i++)
        for (unsigned int j=0; j<3; j++)
            rot_matrix(i,j) = transformation(i,j);

    estimated_pose.setRotationMatrix(rot_matrix);
    estimated_pose.x(transformation(0,3));
    estimated_pose.y(transformation(1,3));
    estimated_pose.z(transformation(2,3));

    for ( size_t i = 0; i < v_obs2.size(); i++ )
    {
        CObservation3DRangeScanPtr obs = v_obs2[i].obs;

        CPose3D pose;
        obs->getSensorPose( pose );

        CPose3D finalPose = estimated_pose + pose;
        obs->setSensorPose(finalPose);
    }

    //cout << "Location set done" << endl;
}

//-----------------------------------------------------------
//                      refineLocationPCL
//-----------------------------------------------------------

void refineLocationPCLBis( vector<CObservation3DRangeScanPtr> &v_obs, vector<CObservation3DRangeScanPtr> &v_obs2 )
{
    CDisplayWindow3D window2("ICP-3D demo: UNALIGNED scans",500,500);
    CDisplayWindow3D window3("ICP-3D demo: ICP-ALIGNED scans",500,500);

    COpenGLScenePtr scene2=COpenGLScene::Create();
    COpenGLScenePtr scene3=COpenGLScene::Create();

    window2.get3DSceneAndLock()=scene2;
    window3.get3DSceneAndLock()=scene3;

    opengl::CGridPlaneXYPtr plane1=CGridPlaneXY::Create(-20,20,-20,20,0,1);
    plane1->setColor(0.3,0.3,0.3);

    scene2->insert(plane1);
    scene3->insert(plane1);

    // Show in Windows:

    window.setCameraElevationDeg(15);
    window.setCameraAzimuthDeg(90);
    window.setCameraZoom(15);

    window2.setCameraElevationDeg(15);
    window2.setCameraAzimuthDeg(90);
    window2.setCameraZoom(15);

    window3.setCameraElevationDeg(15);
    window3.setCameraAzimuthDeg(90);
    window3.setCameraZoom(15);

    // Show the scanned points:
    CSimplePointsMap	M1,M2,M3;

    for ( size_t i = 0; i < v_obs.size(); i++ )
        M1.insertObservationPtr( v_obs[i] );

    for ( size_t i = 0; i < v_obs2.size(); i++ )
        M2.insertObservationPtr( v_obs2[i] );

    cout << "Getting points... points 1: ";

    CVectorDouble xs, ys, zs, xs2, ys2, zs2;
    M1.getAllPoints(xs,ys,zs);
    M2.getAllPoints(xs2,ys2,zs2);

    cout << xs.size() << " points 2: " << xs2.size() << endl;


    CSetOfObjectsPtr  PTNS1 = CSetOfObjects::Create();
    CSetOfObjectsPtr  PTNS2 = CSetOfObjects::Create();
    CSetOfObjectsPtr  PTNS2_ALIGN = CSetOfObjects::Create();

    CPointsMap::COLOR_3DSCENE_R = 1;
    CPointsMap::COLOR_3DSCENE_G = 0;
    CPointsMap::COLOR_3DSCENE_B = 0;
    M1.getAs3DObject(PTNS1);

    CPointsMap::COLOR_3DSCENE_R = 0;
    CPointsMap::COLOR_3DSCENE_G = 0;
    CPointsMap::COLOR_3DSCENE_B = 1;
    M2.getAs3DObject(PTNS2);

    scene2->insert( PTNS1 );
    scene2->insert( PTNS2 );


    scene3->insert( PTNS1 );
    scene3->insert( PTNS2_ALIGN );

    window2.unlockAccess3DScene();
    window2.forceRepaint();
    window3.unlockAccess3DScene();
    window3.forceRepaint();

    //cout << "Inserting points...";

    PointCloud<PointXYZ>::Ptr	cloud_old   (new PointCloud<PointXYZ>());
    PointCloud<PointXYZ>::Ptr	cloud_new   (new PointCloud<PointXYZ>());
    PointCloud<PointXYZ>::Ptr   cloud_trans (new PointCloud<PointXYZ>());

    cloud_old->clear();
    cloud_new->clear();
    cloud_trans->clear();

    for ( size_t i = 0; i < xs.size(); i+=10 )
        cloud_old->push_back(PointXYZ(xs[i],ys[i],zs[i]));

    for ( size_t i = 0; i < xs2.size(); i+=10 )
        cloud_new->push_back(PointXYZ(xs2[i],ys2[i],zs2[i]));

    //cout << "done" << endl;

    // Esa para GICP, si quieres el normal solo le tienes que quitar lo de Generalized
    GeneralizedIterativeClosestPoint<PointXYZ, PointXYZ> gicp;

    //cout << "Setting input clouds...";

    gicp.setInputTarget(cloud_new);
    gicp.setInputTarget(cloud_old);

    //cout << "done"  << endl;
    //cout << "Setting parameters...";

    //ICP options
    gicp.setMaxCorrespondenceDistance (0.5);
    // Set the maximum number of iterations (criterion 1)
    gicp.setMaximumIterations (100);
    // Set the transformation tras epsilon (criterion 2)
    gicp.setTransformationEpsilon (1e-6);
    // Set the transformation rot epsilon (criterion 3)
    gicp.setRotationEpsilon (1e-6);

    //cout << "done"  << endl;

    cout << "Doing ICP...";

    gicp.align(*cloud_trans);

    double score;
    score = gicp.getFitnessScore(); // Returns the squared average error between the aligned input and target

    v_goodness.push_back(score);

    cout << "Done! Average error: " << sqrt(score) << " meters" << endl;

    // Obtain the transformation that aligned cloud_source to cloud_source_registered
    Eigen::Matrix4f transformation = gicp.getFinalTransformation();

    CPose3D estimated_pose;

    //cout << "Transformation" << endl;
    //cout << transformation << endl;

    CMatrixDouble33 rot_matrix;
    for (unsigned int i=0; i<3; i++)
        for (unsigned int j=0; j<3; j++)
            rot_matrix(i,j) = transformation(i,j);

    estimated_pose.setRotationMatrix(rot_matrix);
    estimated_pose.x(transformation(0,3));
    estimated_pose.y(transformation(1,3));
    estimated_pose.z(transformation(2,3));

    CVectorDouble v_pose;
    estimated_pose.getAsVector(v_pose);

    cout << "Pose: " << v_pose << endl;

    //cout << "Pose set done" << endl;

    for ( size_t i = 0; i < v_obs2.size(); i++ )
    {
        CObservation3DRangeScanPtr obs = v_obs2[i];

        CPose3D pose;
        obs->getSensorPose( pose );

        CPose3D finalPose = estimated_pose + pose;
        obs->setSensorPose(finalPose);

        M3.insertObservationPtr( obs );
    }

    CPointsMap::COLOR_3DSCENE_R = 0;
    CPointsMap::COLOR_3DSCENE_G = 1;
    CPointsMap::COLOR_3DSCENE_B = 0;
    M3.getAs3DObject(PTNS2_ALIGN);

    //window2.waitForKey();

    //cout << "Location set done" << endl;
}


//-----------------------------------------------------------
//                      refineLocation
//-----------------------------------------------------------

void refineLocationICP3D( vector<T3DRangeScan> &v_obs, vector<T3DRangeScan> &v_obs2)
{

    if (!initialGuessICP2D)
    {
        for (size_t i=0; i < v_obs2.size(); i++)
        {
            CPose3D pose;
            v_obs[v_obs.size()-v_obs2.size()+i].obs->getSensorPose(pose);
            v_obs2[i].obs->setSensorPose(pose);
        }
    }

    // Show the scanned points:
    CSimplePointsMap	M1,M2;

    cout << "Getting points, points 1: ";

    for ( size_t i = 0; i < v_obs.size(); i++ )
        M1.insertObservationPtr( v_obs[i].obs );

    for ( size_t i = 0; i < v_obs2.size(); i++ )
        M2.insertObservationPtr( v_obs2[i].obs );

    cout << M1.size() << ", points 2: " << M2.size() << " done" << endl;

    COpenGLScenePtr scene1=COpenGLScene::Create();
    COpenGLScenePtr scene2=COpenGLScene::Create();
    COpenGLScenePtr scene3=COpenGLScene::Create();

    opengl::CGridPlaneXYPtr plane1=CGridPlaneXY::Create(-20,20,-20,20,0,1);
    plane1->setColor(0.3,0.3,0.3);
    scene1->insert(plane1);
    scene2->insert(plane1);
    scene3->insert(plane1);

    CSetOfObjectsPtr  PTNS1 = CSetOfObjects::Create();
    CSetOfObjectsPtr  PTNS2 = CSetOfObjects::Create();

    CPointsMap::COLOR_3DSCENE_R = 1;
    CPointsMap::COLOR_3DSCENE_G = 0;
    CPointsMap::COLOR_3DSCENE_B = 0;
    M1.getAs3DObject(PTNS1);

    CPointsMap::COLOR_3DSCENE_R = 0;
    CPointsMap::COLOR_3DSCENE_G = 0;
    CPointsMap::COLOR_3DSCENE_B = 1;
    M2.getAs3DObject(PTNS2);

    scene2->insert( PTNS1 );
    scene2->insert( PTNS2 );

    // --------------------------------------
    // Do the ICP-3D
    // --------------------------------------
    float run_time;
    CICP	icp;
    CICP::TReturnInfo	icp_info;

    icp.options.thresholdDist = 0.40;
    icp.options.thresholdAng = 0;

    CPose3DPDFPtr pdf= icp.Align3D(
                &M2,          // Map to align
                &M1,          // Reference map
                CPose3D() ,   // Initial gross estimate
                &run_time,
                &icp_info);

    CPose3D  mean = pdf->getMeanVal();

    cout << "ICP run took " << run_time << " secs." << endl;
    cout << "Goodness: " << 100*icp_info.goodness << "% , # of iterations= " << icp_info.nIterations << endl;
    cout << "ICP output: mean= " << mean << endl;

    // Aligned maps:
    CSetOfObjectsPtr  PTNS2_ALIGN = CSetOfObjects::Create();

    M2.changeCoordinatesReference( CPose3D()-mean );
    M2.getAs3DObject(PTNS2_ALIGN);

    scene3->insert( PTNS1 );
    scene3->insert( PTNS2_ALIGN );

    // Show in Windows:

    window2.get3DSceneAndLock()=scene2;
    window2.unlockAccess3DScene();

    window3.get3DSceneAndLock()=scene3;
    window3.unlockAccess3DScene();


    mrpt::system::sleep(20);
    window2.forceRepaint();

    window2.setCameraElevationDeg(15);
    window2.setCameraAzimuthDeg(90);
    window2.setCameraZoom(15);

    window3.setCameraElevationDeg(15);
    window3.setCameraAzimuthDeg(90);
    window3.setCameraZoom(15);

    //cout << "Press any key to exit..." << endl;
    //window.waitForKey();

    double goodness = 100*icp_info.goodness;
    v_goodness.push_back( goodness );

    if ( goodness < 96 )
    {
        //return;
        if ( oneGoodICP3DPose )
            mean = lastGoodICP3Dpose;
        else
            mean = CPose3D(0,0,0,0,0,0);

    }

    for ( size_t i = 0; i < v_obs2.size(); i++ )
    {
        CObservation3DRangeScanPtr obs = v_obs2[i].obs;

        CPose3D pose;
        obs->getSensorPose( pose );

        CPose3D finalPose = mean + pose;
        obs->setSensorPose(finalPose);
    }

    oneGoodICP3DPose = true;
    lastGoodICP3Dpose = mean;

}

//-----------------------------------------------------------
//
//                          main
//
//-----------------------------------------------------------

void showUsageInformation()
{
    cout << "Usage information. Two expected arguments: " << endl <<
            " \t (1) Rawlog file." << endl <<
            " \t (2) Points map file." << endl;
    cout << "Then, optional parameters:" << endl <<
            " \t -h             : Shows this help." << endl <<
            " \t -disable_ICP2D : Disable ICP2D as an initial guess for robot localization." << endl <<
            " \t -enable_ICP3D  : Enable ICP3D to refine the RGBD-sensors location." << endl <<
            " \t -enable_GICP3D : Enable GICP3D to refine the RGBD-sensors location." << endl <<
            " \t -enable_memory : Accumulate 3D point clouds already registered." << endl <<
            " \t -enable_smoothing: Enable smoothing of the 3D point clouds." << endl <<
            " \t -enable_keyPoses : Enable the use of key poses only." << endl;
}

//-----------------------------------------------------------
//                          main
//-----------------------------------------------------------

int main(int argc, char **argv)
{
    try
    {
        string simpleMapFile;
        CRawlog i_rawlog, o_rawlog;
        CTicTac clock;
        double time_icp2D = 0, time_icp3D = 0, time_smoothing = 0, time_overlapping = 0;
        vector<string> RGBD_sensors;

        string i_rawlogFile;
        string o_rawlogFile;

        //
        // Load parameters
        //

        if ( argc >= 3 )
        {
            i_rawlogFile = argv[1];
            simpleMapFile = argv[2];

            o_rawlogFile = i_rawlogFile.substr(0,i_rawlogFile.size()-7);

            for ( size_t arg = 3; arg < argc; arg++ )
            {
                if ( !strcmp(argv[arg],"-disable_ICP2D") )
                {
                    initialGuessICP2D = false;
                    cout << "[INFO] Disabled ICP2D to guess the robot localization."  << endl;
                }
                else if ( !strcmp(argv[arg], "-enable_ICP3D") )
                {
                    refineWithICP3D = true;
                    ICP3D_method    = "ICP";  // MRPT
                    cout << "[INFO] Enabled ICP3D."  << endl;
                }
                else if ( !strcmp(argv[arg], "-enable_GICP3D") )
                {
                    refineWithICP3D = true;
                    ICP3D_method     = "GICP"; // PCL
                    cout << "[INFO] Enabled GICP3D."  << endl;
                }
                else if ( !strcmp(argv[arg], "-enable_memory") )
                {
                    accumulatePast   = true;
                    cout << "[INFO] Enabled (G)ICP3D memory."  << endl;
                }
                else if ( !strcmp(argv[arg], "-enable_keyPoses") )
                {
                    useKeyPoses = true;
                    cout << "[INFO] Enabled key poses."  << endl;
                }
                else if ( !strcmp(argv[arg], "-enable_overlapping") )
                {
                    useOverlappingObs = true;
                    cout << "[INFO] Enabled overlapping obs."  << endl;
                }
                else if ( !strcmp(argv[arg], "-enable_smoothing") )
                {
                    smooth3DObs  = true;
                    cout << "[INFO] Enabled smoothing."  << endl;
                }
                else if ( !strcmp(argv[arg], "-h") )
                {
                    showUsageInformation();
                    return 0;
                }
                else
                {
                    cout << "[Error] " << argv[arg] << "unknown paramter" << endl;
                    showUsageInformation();
                    return -1;
                }

            }
        }
        else
        {
            showUsageInformation();

            return 0;
        }

        if ( refineWithICP3D && ICP3D_method == "GICP" )
            o_rawlogFile += "_located-GICP";
        else if ( refineWithICP3D && ICP3D_method == "ICP" )
            o_rawlogFile += "_located-ICP";
        else
            o_rawlogFile += "_located";

        if ( refineWithICP3D && accumulatePast )
            o_rawlogFile += "-memory";

        if ( smooth3DObs )
            o_rawlogFile += "-smoothed";

        o_rawlogFile += ".rawlog";


        if (!i_rawlog.loadFromRawLogFile(i_rawlogFile))
            throw std::runtime_error("Couldn't open rawlog dataset file for input...");

        cout << "[INFO] Working with " << i_rawlogFile << endl;

        // Create the reference objects:

        if ( refineWithICP3D )
        {
            if ( ICP3D_method == "GICP")
            {
                window.setPos(-500,-500);
                window2.setPos(-500,-500);
                window3.setPos(-500,-500);
            }
            else
            {
                window.setPos(10,10);
                window2.setPos(530,10);
                window3.setPos(10,520);
            }
        }
        else
        {
            window.setPos(-500,-500);
            window2.setPos(-500,-500);
            window3.setPos(-500,-500);
        }

        //
        // Compute initial guess from ICP2D
        //

        cout << "---------------------------------------------------" << endl;
        cout << "        Computing initial poses with ICP2D" << endl;
        cout << "---------------------------------------------------" << endl;

        if ( initialGuessICP2D )
        {
            clock.Tic();

            for ( size_t obsIndex = 0; obsIndex < i_rawlog.size(); obsIndex++ )
            {
                CObservationPtr obs = i_rawlog.getAsObservation(obsIndex);

                if ( IS_CLASS(obs, CObservation2DRangeScan) )
                {
                    CObservation2DRangeScanPtr obs2D = CObservation2DRangeScanPtr(obs);
                    obs2D->load();
                    double goodness;

                    trajectoryICP2D(simpleMapFile,i_rawlog,obs2D,goodness);

                    TRobotPose robotPose;
                    robotPose.pose = initialPose;
                    robotPose.time = obs2D->timestamp;
                    robotPose.goodness = goodness;

                    if ( goodness > 80 )
                        v_robotPoses.push_back( robotPose );
                    else
                        v_pending3DRangeScans.clear();

                    // Process pending 3D range scans, if any
                    if ( !v_pending3DRangeScans.empty() )
                        processPending3DRangeScans();
                }
                else //if ( obs->sensorLabel == RGBD_sensor )
                {
                    string &label = obs->sensorLabel;

                    if ( find(RGBD_sensors.begin(), RGBD_sensors.end(), label)
                         == RGBD_sensors.end() )
                        RGBD_sensors.push_back(label);

                    CObservation3DRangeScanPtr obs3D = CObservation3DRangeScanPtr(obs);
                    obs3D->load();

                    v_pending3DRangeScans.push_back( obs3D );
                }
                /*else
                    continue;*/
            }

            time_icp2D = clock.Tac();

            trajectoryFile.close();
        }
        else
        {
            for ( size_t obsIndex = 0; obsIndex < i_rawlog.size(); obsIndex++ )
            {
                CObservationPtr obs = i_rawlog.getAsObservation(obsIndex);

                if ( IS_CLASS(obs, CObservation2DRangeScan) )
                {

                }
                else
                {
                    CObservation3DRangeScanPtr obs3D = CObservation3DRangeScanPtr(obs);
                    obs3D->load();

                    T3DRangeScan obs;
                    obs.obs = obs3D;

                    v_3DRangeScans.push_back( obs );
                }
            }
        }

        //
        // Smooth3DObs?
        //

        if ( smooth3DObs )
        {
            cout << "[INFO] Smoothing point clouds... ";

            clock.Tic();

            for ( size_t i = 0; i < v_3DRangeScans.size(); i++ )
                smoothObs( v_3DRangeScans[i].obs );

            time_smoothing = clock.Tac();

            cout << " done." << endl;
        }

        //
        // Use overlapping
        //

        if ( useOverlappingObs )
        {
            cout << "[INFO] Computing convex hulls to check overlapping... ";

            clock.Tic();

            for ( size_t i = 0; i < v_3DRangeScans.size(); i++ )
            {
                // Get point cloud of old observation
                pcl::PointCloud<pcl::PointXYZ>::Ptr pointCloud ( new pcl::PointCloud<pcl::PointXYZ>());
                v_3DRangeScans[i].obs->project3DPointsFromDepthImageInto(*pointCloud,true);

                pcl::ConvexHull<pcl::PointXYZ> convex_hull;
                convex_hull.setInputCloud(pointCloud);
                convex_hull.setDimension(3);
                convex_hull.reconstruct(*v_3DRangeScans[i].convexHullCloud,
                                        v_3DRangeScans[i].polygons);
            }

            time_overlapping = clock.Tac();

            cout << " done." << endl;
        }

        //
        // Refine using ICP3D
        //

        if ( refineWithICP3D )
        {
            clock.Tic();

            size_t N_scans = v_3DRangeScans.size();

            /*vector< vector <CObservation3DRangeScanPtr> > v_allObs(4);  // Past set of obs

            for ( size_t obsIndex = 0; obsIndex < N_scans; obsIndex++ )
            {
                CObservation3DRangeScanPtr obs = v_3DRangeScans[obsIndex];

                if ( obs->sensorLabel == "RGBD_1" )
                {
                    v_allObs[0].push_back(obs);
                }
                else if ( obs->sensorLabel == "RGBD_2" )
                {
                    v_allObs[1].push_back(obs);
                }
                else if ( obs->sensorLabel == "RGBD_3" )
                {
                    v_allObs[2].push_back(obs);
                }
                else if ( obs->sensorLabel == "RGBD_4" )
                {
                    v_allObs[3].push_back(obs);
                }
            }

            refineLocationPCLBis( v_allObs[0], v_allObs[1] );
            refineLocationPCLBis( v_allObs[0], v_allObs[2] );
            refineLocationPCLBis( v_allObs[0], v_allObs[3] );*/

            cout << "---------------------------------------------------" << endl;
            cout << "         Refining sensor poses using " << ICP3D_method << endl;
            cout << "---------------------------------------------------" << endl;

            if ( RGBD_sensors.size() == 4 )
            {

                vector<T3DRangeScan> v_obs;  // Past set of obs
                vector<T3DRangeScan> v_obs2(4); // Current set of obs
                vector<size_t> v_obs2_indices(4);            // Indices in v_3DRangeScans vector of the current set of obs
                vector<size_t> v_obsToDelete; // If using key poses, not key obs to delete
                vector<bool> v_obs_loaded(4,false);

                bool first = true;
                size_t set_index = 0;

                CTicTac clockEllapsedICP3D;
                clockEllapsedICP3D.Tic();

                for ( size_t obsIndex = 0; obsIndex < N_scans; obsIndex++ )
                {
                    CTicTac clockLoop;
                    clockLoop.Tic();

                    T3DRangeScan obs = v_3DRangeScans[obsIndex];

                    if ( obs.obs->sensorLabel == "RGBD_1" )
                    {
                        v_obs2[0]       = obs;
                        v_obs_loaded[0] = true;
                        v_obs2_indices[0] = obsIndex;
                    }
                    else if ( obs.obs->sensorLabel == "RGBD_2" )
                    {
                        v_obs2[1]       = obs;
                        v_obs_loaded[1] = true;
                        v_obs2_indices[1] = obsIndex;
                    }
                    else if ( obs.obs->sensorLabel == "RGBD_3" )
                    {
                        v_obs2[2]       = obs;
                        v_obs_loaded[2] = true;
                        v_obs2_indices[2] = obsIndex;
                    }
                    else if ( obs.obs->sensorLabel == "RGBD_4" )
                    {
                        v_obs2[3]       = obs;
                        v_obs_loaded[3] = true;
                        v_obs2_indices[3] = obsIndex;
                    }

                    double sum;

                    sum = v_obs_loaded[0] + v_obs_loaded[1] + v_obs_loaded[2] + v_obs_loaded[3];

                    if ( sum == 4 )
                    {
                        v_obs_loaded.clear();
                        v_obs_loaded.resize(4,false);

                        cout << "Working set of obs index... " << set_index++ << " of approx. " << (double)N_scans/4 << endl;

                        if ( first )
                        {
                            v_obs = v_obs2;
                            first = false;
                            continue;
                        }
                        else
                        {
                            // Check if it's a key pose
                            if ( useKeyPoses )
                            {
                                bool keyPose = true;

                                CPose3D pose1,pose2,pose3,pose4;
                                CPose3D pose2_1,pose2_2,pose2_3,pose2_4;

                                size_t N_prevObs = v_obs.size();

                                v_obs[N_prevObs-4].obs->getSensorPose(pose1);
                                v_obs[N_prevObs-3].obs->getSensorPose(pose2);
                                v_obs[N_prevObs-2].obs->getSensorPose(pose3);
                                v_obs[N_prevObs-1].obs->getSensorPose(pose4);

                                v_obs2[0].obs->getSensorPose(pose2_1);
                                v_obs2[1].obs->getSensorPose(pose2_2);
                                v_obs2[2].obs->getSensorPose(pose2_3);
                                v_obs2[3].obs->getSensorPose(pose2_4);

                                keyPose = keyPose && isKeyPose(pose2_1,pose1);
                                keyPose = keyPose && isKeyPose(pose2_2,pose2);
                                keyPose = keyPose && isKeyPose(pose2_3,pose3);
                                keyPose = keyPose && isKeyPose(pose2_4,pose4);

                                if (!keyPose)
                                {
                                    //                                cout << "Delete: " << v_obs2_indices[0] << " " << v_obs2_indices[1] << " " <<
                                    //                                        v_obs2_indices[2] << " " << v_obs2_indices[3] << endl;

                                    cout << "Not a key pose, moving to the next one." << endl;
                                    cout << "---------------------------------------------------" << endl;

                                    v_obsToDelete.push_back(v_obs2_indices[0]);
                                    v_obsToDelete.push_back(v_obs2_indices[1]);
                                    v_obsToDelete.push_back(v_obs2_indices[2]);
                                    v_obsToDelete.push_back(v_obs2_indices[3]);
                                    continue;
                                }
                            }
                        }

                        vector<T3DRangeScan>  v1(1,v_obs2[0]);
                        vector<T3DRangeScan>  v2(1,v_obs2[1]);
                        vector<T3DRangeScan>  v3(1,v_obs2[2]);
                        vector<T3DRangeScan>  v4(1,v_obs2[3]);

                        if ( ICP3D_method == "GICP" )
                        {
                            if ( accumulatePast )
                            {
                                refineLocationGICP3D( v_obs, v1  );
                                refineLocationGICP3D( v_obs, v2  );
                                refineLocationGICP3D( v_obs, v3  );
                                refineLocationGICP3D( v_obs, v4  );
                            }
                            else
                                refineLocationGICP3D( v_obs, v_obs2 );
                        }
                        else // ICP3D
                        {
                            if ( accumulatePast )
                            {
                                refineLocationICP3D( v_obs, v1  );
                                refineLocationICP3D( v_obs, v2  );
                                refineLocationICP3D( v_obs, v3  );
                                refineLocationICP3D( v_obs, v4  );
                            }
                            else
                                refineLocationICP3D( v_obs, v_obs2 );
                        }

                        //                    CPose3D pose1, pose2,pose3,pose4;

                        //                    v_obs2[0]->getSensorPose(pose1);
                        //                    v_obs2[1]->getSensorPose(pose2);
                        //                    v_obs2[2]->getSensorPose(pose3);
                        //                    v_obs2[3]->getSensorPose(pose4);

                        //                    cout << "Pose 1 results: " << pose1 << endl;
                        //                    cout << "Pose 2 results: " << pose2 << endl;
                        //                    cout << "Pose 3 results: " << pose3 << endl;
                        //                    cout << "Pose 4 results: " << pose4 << endl;

                        if ( accumulatePast )
                            v_obs.insert(v_obs.end(), v_obs2.begin(), v_obs2.end() );
                        else
                            v_obs = v_obs2;

                        // Time Statistics

                        cout << "Time ellapsed      : " <<
                                size_t(clockLoop.Tac()  / SECS_PER_MIN) << " min. " <<
                                size_t(clockLoop.Tac()) % SECS_PER_MIN << " s." << endl;

                        cout << "Total time ellapsed: " <<
                                size_t(clockEllapsedICP3D.Tac()  / SECS_PER_MIN) << " min. " <<
                                size_t(clockEllapsedICP3D.Tac()) % SECS_PER_MIN  << " s." << endl;

                        cout << "---------------------------------------------------" << endl;
                    }
                }
            }
            else
            {
                vector<T3DRangeScan> v_obs;  // Past obs
                vector<T3DRangeScan> v_obs2(1); // Current obs

                bool first = true;
                size_t set_index = 0;

                CTicTac clockEllapsedICP3D;
                clockEllapsedICP3D.Tic();

                for ( size_t obsIndex = 0; obsIndex < N_scans; obsIndex++ )
                {
                    CTicTac clockLoop;
                    clockLoop.Tic();

                    T3DRangeScan obs = v_3DRangeScans[obsIndex];

                    v_obs2[0] = obs;

                    cout << "Working obs index... " << set_index++ << " of approx. " << N_scans << endl;

                    if ( first )
                    {
                        v_obs = v_obs2;
                        first = false;
                        continue;
                    }
                    else
                    {

                        vector<T3DRangeScan>  v1(1,v_obs2[0]);

                        if ( ICP3D_method == "GICP" )
                        {
                            if ( accumulatePast )
                            {
                                refineLocationGICP3D( v_obs, v1  );
                            }
                            else
                                refineLocationGICP3D( v_obs, v_obs2 );
                        }
                        else // ICP3D
                        {
                            if ( accumulatePast )
                            {
                                refineLocationICP3D( v_obs, v1  );
                            }
                            else
                                refineLocationICP3D( v_obs, v_obs2 );
                        }

                        if ( accumulatePast )
                            v_obs.insert(v_obs.end(), v_obs2.begin(), v_obs2.end() );
                        else
                            v_obs = v_obs2;

                        // Time Statistics

                        cout << "Time ellapsed      : " <<
                                size_t(clockLoop.Tac()  / SECS_PER_MIN) << " min. " <<
                                size_t(clockLoop.Tac()) % SECS_PER_MIN << " s." << endl;

                        cout << "Total time ellapsed: " <<
                                size_t(clockEllapsedICP3D.Tac()  / SECS_PER_MIN) << " min. " <<
                                size_t(clockEllapsedICP3D.Tac()) % SECS_PER_MIN  << " s." << endl;

                        cout << "---------------------------------------------------" << endl;
                    }
                }

                cout << "Mean goodness: "
                     << std::accumulate(v_goodness.begin(), v_goodness.end(), 0.0 ) / v_goodness.size()
                     << endl;

                cout << " done." << endl;

                time_icp3D = clock.Tac();

            }
        }

        TTimeStamp totalTime;
        TTimeParts totalTimeParts;
        totalTime = secondsToTimestamp(time_icp3D);
        timestampToParts(totalTime,totalTimeParts);

        cout << "---------------------------------------------------" << endl;
        cout << "                 Time statistics" << endl;
        cout << "---------------------------------------------------" << endl;
        cout << "[INFO] time spent by the icp2D process: " << time_icp2D << " sec." << endl;
        cout << "[INFO] time spent smoothing           : " << time_smoothing << " sec." << endl;
        cout << "[INFO] time spent computing hulls     : " << time_overlapping << " sec." << endl;
        cout << "[INFO] time spent by the icp3D process: " ;

        if ( time_icp3D )
        {
            cout << totalTimeParts.hour << " hours " <<
                    totalTimeParts.minute << " min. " <<
                    totalTimeParts.second << " sec." << endl;
        }
        else
            cout << time_icp3D << " sec." << endl;

        cout << "---------------------------------------------------" << endl;

        cout << "[INFO] Saving obs to rawlog file " << o_rawlogFile << " ...";

        for ( size_t obs_index = 0; obs_index < v_3DRangeScans.size(); obs_index++ )
        {
            // Restore point cloud
            if ( smooth3DObs )
                v_3DRangeScans[obs_index].obs->project3DPointsFromDepthImage();

            CSensoryFrame SF;
            SF.insert( v_3DRangeScans[obs_index].obs );

            o_rawlog.addObservationMemoryReference( v_3DRangeScans[obs_index].obs );
        }

        o_rawlog.saveToRawLogFile( o_rawlogFile );

        cout << " completed." << endl;

        //
        // Visualize 2D results
        //

        if ( visualize2DResults )
        {
            win.hold_on();

            CVectorDouble coord_x, coord_y;
            for ( size_t pos_index = 0; pos_index < v_robotPoses.size(); pos_index++ )
            {
                CVectorDouble v_coords;
                v_robotPoses[pos_index].pose.getAsVector(v_coords);
                coord_x.push_back(v_coords[0]);
                coord_y.push_back(v_coords[1]);
            }

            win.plot(coord_x,coord_y,"-m.3");
            win.plot(coord_x,coord_y,"k.9");

            CVectorDouble coord_x2, coord_y2;

            for ( size_t pos_index = 0; pos_index < v_3DRangeScans.size(); pos_index++ )
            {
                CPose3D pose;
                CVectorDouble v_coords;
                v_3DRangeScans[pos_index].obs ->getSensorPose(pose);
                pose.getAsVector(v_coords);
                coord_x2.push_back(v_coords[0]);
                coord_y2.push_back(v_coords[1]);
            }

            win.plot(coord_x2,coord_y2,"g.8");

            win.waitForKey();

        }

        return 0;

    } catch (exception &e)
    {
        cout << "HOMe exception caught: " << e.what() << endl;
        return -1;
    }
    catch (...)
    {
        printf("Another exception!!");
        return -1;
    }
}
