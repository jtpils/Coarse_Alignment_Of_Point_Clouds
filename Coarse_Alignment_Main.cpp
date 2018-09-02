#include <limits>
#include <fstream>
#include <vector>
#include <Eigen/Core>
#include <pcl/point_types.h>
#include <pcl/point_cloud.h>
#include <pcl/io/pcd_io.h>
#include <pcl/kdtree/kdtree_flann.h>
#include <pcl/filters/passthrough.h>
#include <pcl/filters/voxel_grid.h>
#include <pcl/features/normal_3d.h>
#include <pcl/features/fpfh.h>
#include <pcl/registration/ia_ransac.h>
#include <pcl/registration/icp.h>

#include <pcl/kdtree/kdtree_flann.h>
#include <pcl/surface/gp3.h>

#include <pcl/surface/mls.h>

#include <pcl\PCLPointCloud2.h>
#include <pcl\segmentation\sac_segmentation.h>
#include <pcl/filters/extract_indices.h>
#include <pcl\common\time.h>
#include <pcl/registration/transformation_estimation_svd.h>
#include <pcl\filters\statistical_outlier_removal.h>
#include <pcl\filters\radius_outlier_removal.h>
#include <pcl/visualization/cloud_viewer.h>
#include <pcl/segmentation/region_growing.h>
#include <pcl\keypoints\harris_3d.h>

//sift
#include <pcl/common/io.h>
#include <pcl/keypoints/sift_keypoint.h>

//kdtree
#include <iostream>
#include <ctime>


class FeatureCloud
{
public:
	// A bit of shorthand
	typedef pcl::PointCloud<pcl::PointXYZ> PointCloud;
	typedef pcl::PointCloud<pcl::Normal> SurfaceNormals;
	typedef pcl::PointCloud<pcl::FPFHSignature33> LocalFeatures;
	typedef pcl::search::KdTree<pcl::PointXYZ> SearchMethod;

	FeatureCloud() :
		search_method_xyz_(new SearchMethod),
		normal_radius_(0.02f),
		feature_radius_(0.02f)
	{}

	~FeatureCloud() {}

	// Process the given cloud
	void
		setInputCloud(PointCloud::Ptr xyz)
	{
		xyz_ = xyz;
		processInput();
	}

	// Load and process the cloud in the given PCD file
	void
		loadInputCloud(const std::string &pcd_file)
	{
		xyz_ = PointCloud::Ptr(new PointCloud);
		pcl::io::loadPCDFile(pcd_file, *xyz_);
		processInput();
	}

	// Get a pointer to the cloud 3D points
	PointCloud::Ptr
		getPointCloud() const
	{
		return (xyz_);
	}

	// Get a pointer to the cloud of 3D surface normals
	SurfaceNormals::Ptr
		getSurfaceNormals() const
	{
		return (normals_);
	}

	// Get a pointer to the cloud of feature descriptors
	LocalFeatures::Ptr
		getLocalFeatures() const
	{
		return (features_);
	}

protected:
	// Compute the surface normals and local features
	void
		processInput()
	{
		computeSurfaceNormals();
		computeLocalFeatures();
	}

	// Compute the surface normals
	void
		computeSurfaceNormals()
	{
		normals_ = SurfaceNormals::Ptr(new SurfaceNormals);

		pcl::NormalEstimation<pcl::PointXYZ, pcl::Normal> norm_est;
		norm_est.setInputCloud(xyz_);
		norm_est.setSearchMethod(search_method_xyz_);
		norm_est.setRadiusSearch(normal_radius_);
		norm_est.compute(*normals_);
	}

	// Compute the local feature descriptors
	void
		computeLocalFeatures()
	{
		features_ = LocalFeatures::Ptr(new LocalFeatures);

		pcl::FPFHEstimation<pcl::PointXYZ, pcl::Normal, pcl::FPFHSignature33> fpfh_est;
		fpfh_est.setInputCloud(xyz_);
		fpfh_est.setInputNormals(normals_);
		fpfh_est.setSearchMethod(search_method_xyz_);
		fpfh_est.setRadiusSearch(feature_radius_);
		fpfh_est.compute(*features_);
	}

private:
	// Point cloud data
	PointCloud::Ptr xyz_;
	SurfaceNormals::Ptr normals_;
	LocalFeatures::Ptr features_;
	SearchMethod::Ptr search_method_xyz_;

	// Parameters
	float normal_radius_;
	float feature_radius_;
};

class TemplateAlignment
{
public:

	// A struct for storing alignment results
	struct Result
	{
		float fitness_score;
		Eigen::Matrix4f final_transformation;
		EIGEN_MAKE_ALIGNED_OPERATOR_NEW
	};

	TemplateAlignment() :
		min_sample_distance_(0.05f),
		max_correspondence_distance_(0.01f*0.01f),
		nr_iterations_(500)
	{
		// Initialize the parameters in the Sample Consensus Initial Alignment (SAC-IA) algorithm
		sac_ia_.setMinSampleDistance(min_sample_distance_);
		sac_ia_.setMaxCorrespondenceDistance(max_correspondence_distance_);
		sac_ia_.setMaximumIterations(nr_iterations_);
	}

	~TemplateAlignment() {}

	// Set the given cloud as the target to which the templates will be aligned
	void
		setTargetCloud(FeatureCloud &target_cloud)
	{
		target_ = target_cloud;
		sac_ia_.setInputTarget(target_cloud.getPointCloud());
		sac_ia_.setTargetFeatures(target_cloud.getLocalFeatures());
	}

	// Add the given cloud to the list of template clouds
	void
		addTemplateCloud(FeatureCloud &template_cloud)
	{
		templates_.push_back(template_cloud);
	}

	// Align the given template cloud to the target specified by setTargetCloud ()
	void
		align(FeatureCloud &template_cloud, TemplateAlignment::Result &result)
	{
		sac_ia_.setInputCloud(template_cloud.getPointCloud());
		sac_ia_.setSourceFeatures(template_cloud.getLocalFeatures());

		pcl::PointCloud<pcl::PointXYZ> registration_output;
		sac_ia_.align(registration_output);

		result.fitness_score = (float)sac_ia_.getFitnessScore(max_correspondence_distance_);
		result.final_transformation = sac_ia_.getFinalTransformation();
	}

	// Align all of template clouds set by addTemplateCloud to the target specified by setTargetCloud ()
	void
		alignAll(std::vector<TemplateAlignment::Result, Eigen::aligned_allocator<Result> > &results)
	{
		results.resize(templates_.size());
		for (size_t i = 0; i < templates_.size(); ++i)
		{
			align(templates_[i], results[i]);
		}
	}

	// Align all of template clouds to the target cloud to find the one with best alignment score
	int
		findBestAlignment(TemplateAlignment::Result &result)
	{
		// Align all of the templates to the target cloud
		std::vector<Result, Eigen::aligned_allocator<Result> > results;
		alignAll(results);

		// Find the template with the best (lowest) fitness score
		float lowest_score = std::numeric_limits<float>::infinity();
		int best_template = 0;
		for (size_t i = 0; i < results.size(); ++i)
		{
			const Result &r = results[i];
			if (r.fitness_score < lowest_score)
			{
				lowest_score = r.fitness_score;
				best_template = (int)i;
			}
		}

		// Output the best alignment
		result = results[best_template];
		return (best_template);
	}

private:
	// A list of template clouds and the target to which they will be aligned
	std::vector<FeatureCloud> templates_;
	FeatureCloud target_;

	// The Sample Consensus Initial Alignment (SAC-IA) registration routine and its parameters
	pcl::SampleConsensusInitialAlignment<pcl::PointXYZ, pcl::PointXYZ, pcl::FPFHSignature33> sac_ia_;
	float min_sample_distance_;
	float max_correspondence_distance_;
	int nr_iterations_;
};

// Align a collection of object templates to a sample point cloud
int
main(int argc, char **argv)
{

	//switch program
	int compute_transmatrix = 0;
	int do_transformation = 0;
	int do_template_match = 0;
	int do_filter = 0;
	int do_keypoint_detect = 0;
	int do_plane_segmentation = 0;
	int do_smoothing = 0;
	int do_normal_estimation = 0;
	int do_sift_keypoint = 0;
	int do_kdtree_search = 1;
	int do_icp = 0;



	//defining variables
	Eigen::Matrix4f transform_mat = Eigen::Matrix4f::Identity();




	////load point cloud files
	if (argc < 3)
	{
		printf("No target PCD file given in command line argument!\n");
		return (-1);
	}
	pcl::PointCloud<pcl::PointXYZ>::Ptr orig_pcd(new pcl::PointCloud<pcl::PointXYZ>);
	pcl::PointCloud<pcl::PointXYZ>::Ptr trans_pcd(new pcl::PointCloud<pcl::PointXYZ>);
	
	pcl::PointCloud<pcl::PointXYZI>::Ptr orig_intens(new pcl::PointCloud<pcl::PointXYZI>);
	pcl::io::loadPCDFile(argv[4], *orig_pcd);
	/*pcl::io::loadPCDFile(argv[2], *trans_pcd);*/
	//pcl::io::savePCDFile("wp2_pyreadable.pcd", *orig_pcd);
	//std::cout << "pcd file loaded";


	//forming transformation matrix
	if (do_transformation == 1)
	{
		//z = 50
		transform_mat(0, 0) = 0.6427876;
		transform_mat(0, 1) = -0.7660444;
		transform_mat(0, 2) = 0.0;
		transform_mat(1, 0) = 0.7660444;
		transform_mat(1, 1) = 0.6427876;
		transform_mat(1, 2) = 0.0;
		transform_mat(2, 0) = 0.0;
		transform_mat(2, 1) = 0.0;
		transform_mat(2, 2) = 1.0;
		//translational value below:
		transform_mat(0, 3) = 0.0;
		transform_mat(1, 3) = 0.0;
		transform_mat(2, 3) = 0.0;

		transform_mat(3, 3) = 1.0;
		pcl::transformPointCloud(*orig_pcd, *trans_pcd, transform_mat);
		pcl::io::savePCDFile("single_plane_transformed.pcd", *trans_pcd);
	}

	//to find transformation matrix
	if (compute_transmatrix == 1)
	{
		pcl::PointCloud<pcl::PointXYZ>::Ptr sr_pcd(new pcl::PointCloud<pcl::PointXYZ>);
		pcl::PointCloud<pcl::PointXYZ>::Ptr tr_pcd(new pcl::PointCloud<pcl::PointXYZ>);
		pcl::io::loadPCDFile(argv[1], *sr_pcd);
		//pcl::io::savePCDFile("pyreadable_auto.pcd", *sr_pcd);
		pcl::io::loadPCDFile(argv[2], *tr_pcd);
		pcl::io::savePCDFile("pyreadable_auto_icp_aligned_pose0.pcd", *tr_pcd);
		pcl::registration::TransformationEstimationSVD<pcl::PointXYZ, pcl::PointXYZ> TESVD;
		pcl::registration::TransformationEstimationSVD<pcl::PointXYZ, pcl::PointXYZ>::Matrix4 transformation2;
		TESVD.estimateRigidTransformation(*sr_pcd, *tr_pcd, transformation2);
		std::cout << "The Estimated Rotation and translation matrices(using getTransformation function) are : \n" << std::endl;
		printf("\n");
		printf(" | %6.3f %6.3f %6.3f | \n", transformation2(0, 0), transformation2(0, 1), transformation2(0, 2));
		printf("R = | %6.3f %6.3f %6.3f | \n", transformation2(1, 0), transformation2(1, 1), transformation2(1, 2));
		printf(" | %6.3f %6.3f %6.3f | \n", transformation2(2, 0), transformation2(2, 1), transformation2(2, 2));
		printf("\n");
		printf("t = < %0.3f, %0.3f, %0.3f >\n", transformation2(0, 3), transformation2(1, 3), transformation2(2, 3));
		//pcl::PointCloud<pcl::PointXYZ>::Ptr temp_pcd(new pcl::PointCloud<pcl::PointXYZ>);
		//pcl::transformPointCloud(*orig_pcd, *temp_pcd, transformation2);
		//pcl::io::savePCDFile("transformed_temp.pcd", *temp_pcd);

	}

	if (do_template_match == 1)
	{

		// Load the object templates specified in the object_templates.txt file
		std::vector<FeatureCloud> object_templates;
		std::ifstream input_stream(argv[3]);
		object_templates.resize(0);
		std::string pcd_filename;
		while (input_stream.good())
		{
			std::getline(input_stream, pcd_filename);
			if (pcd_filename.empty() || pcd_filename.at(0) == '#') // Skip blank lines or comments
				continue;

			FeatureCloud template_cloud;
			template_cloud.loadInputCloud(pcd_filename);
			object_templates.push_back(template_cloud);
		}
		input_stream.close();

		// Load the target cloud PCD file
		pcl::PointCloud<pcl::PointXYZ>::Ptr cloud(new pcl::PointCloud<pcl::PointXYZ>);
		pcl::io::loadPCDFile(argv[2], *cloud);

		// Preprocess the cloud by...
		// ...removing distant points
		//const float depth_limit = 1.0;
		//pcl::PassThrough<pcl::PointXYZ> pass;
		//pass.setInputCloud(cloud);
		//pass.setFilterFieldName("z");
		//pass.setFilterLimits(0, depth_limit);
		//pass.filter(*cloud);

		// ... and downsampling the point cloud
		const float voxel_grid_size = 5.0f;
		pcl::VoxelGrid<pcl::PointXYZ> vox_grid;
		vox_grid.setInputCloud(cloud);
		vox_grid.setLeafSize(voxel_grid_size, voxel_grid_size, voxel_grid_size);
		//vox_grid.filter (*cloud); // Please see this http://www.pcl-developers.org/Possible-problem-in-new-VoxelGrid-implementation-from-PCL-1-5-0-td5490361.html
		pcl::PointCloud<pcl::PointXYZ>::Ptr tempCloud(new pcl::PointCloud<pcl::PointXYZ>);
		vox_grid.filter(*tempCloud);
		cloud = tempCloud;

		// Assign to the target FeatureCloud
		FeatureCloud target_cloud;
		target_cloud.setInputCloud(cloud);

		// Set the TemplateAlignment inputs
		TemplateAlignment template_align;
		for (size_t i = 0; i < object_templates.size(); ++i)
		{
			template_align.addTemplateCloud(object_templates[i]);
		}
		template_align.setTargetCloud(target_cloud);

		// Find the best template alignment
		TemplateAlignment::Result best_alignment;
		int best_index = template_align.findBestAlignment(best_alignment);
		const FeatureCloud &best_template = object_templates[best_index];

		// Print the alignment fitness score (values less than 0.00002 are good)
		printf("Best fitness score: %f\n", best_alignment.fitness_score);

		// Print the rotation matrix and translation vector
		Eigen::Matrix3f rotation = best_alignment.final_transformation.block<3, 3>(0, 0);
		Eigen::Vector3f translation = best_alignment.final_transformation.block<3, 1>(0, 3);

		printf("\n");
		printf("    | %6.3f %6.3f %6.3f | \n", rotation(0, 0), rotation(0, 1), rotation(0, 2));
		printf("R = | %6.3f %6.3f %6.3f | \n", rotation(1, 0), rotation(1, 1), rotation(1, 2));
		printf("    | %6.3f %6.3f %6.3f | \n", rotation(2, 0), rotation(2, 1), rotation(2, 2));
		printf("\n");
		printf("t = < %0.3f, %0.3f, %0.3f >\n", translation(0), translation(1), translation(2));

		// Save the aligned template for visualization
		pcl::PointCloud<pcl::PointXYZ> transformed_cloud;
		pcl::transformPointCloud(*best_template.getPointCloud(), transformed_cloud, best_alignment.final_transformation);
		pcl::io::savePCDFileBinary("output.pcd", transformed_cloud);
	}

	if (do_filter == 1)
	{
		//const float depth_limit_end_z = 1640.0;
		//const float depth_limit_start_z = 1400.0;
		//pose0down


		const float depth_limit_end_x = 125.24;
		const float depth_limit_start_x = -50.0;
		pcl::PassThrough<pcl::PointXYZ> pass_x;
		pass_x.setInputCloud(orig_pcd);
		pass_x.setFilterFieldName("x");
		pass_x.setFilterLimits(depth_limit_start_x, depth_limit_end_x);
		pass_x.filter(*orig_pcd);

		//////const float depth_limit_end_y = -187.0;
		//////const float depth_limit_start_y = 58.0;
		//pcl::io::savePCDFile("after_filter.pcd", *orig_pcd);

		const float depth_limit_end_z = 690.0;
		const float depth_limit_start_z = 500.0;
		pcl::PassThrough<pcl::PointXYZ> pass_z;
		pass_z.setInputCloud(orig_pcd);
		pass_z.setFilterFieldName("z");
		pass_z.setFilterLimits(depth_limit_start_z, depth_limit_end_z);
		pass_z.filter(*orig_pcd);
		pcl::io::savePCDFile("after_filter.pcd", *orig_pcd);
		//pose0down
		////pose0down
		const float depth_limit_end_y = -125.0 ;
		const float depth_limit_start_y = 70.0;
		pcl::PassThrough<pcl::PointXYZ> pass_y;
		pass_y.setInputCloud(orig_pcd);
		pass_y.setFilterFieldName("y");
		pass_y.setFilterLimits(depth_limit_end_y, depth_limit_start_y);
		pass_y.filter(*orig_pcd);
		pcl::io::savePCDFile("after_filter.pcd", *orig_pcd);

	}

	if (do_keypoint_detect == 1)
	{

		pcl::HarrisKeypoint3D<pcl::PointXYZ, pcl::PointXYZI>* harris3D = new
			pcl::HarrisKeypoint3D<pcl::PointXYZ, pcl::PointXYZI>(pcl::HarrisKeypoint3D<pcl::PointXYZ, pcl::PointXYZI>::HARRIS);

		harris3D->setNonMaxSupression(false);
		harris3D->setRadius(12);
		harris3D->setInputCloud(orig_pcd);
		pcl::PointCloud<pcl::PointXYZI>::Ptr keypoints(new pcl::PointCloud<pcl::PointXYZI>);
		pcl::PointCloud<pcl::PointXYZ>::Ptr key_regions(new pcl::PointCloud<pcl::PointXYZ>);
		harris3D->compute(*keypoints);
		pcl::StopWatch watch;
		pcl::console::print_highlight("Detected %zd points in %lfs\n", keypoints->size(), watch.getTimeSeconds());

		/*if (!keypoints->size())
		{*/
		pcl::io::savePCDFile("keypoints_10r_cad_pose0.pcd", *keypoints);
		pcl::console::print_info("Saved keypoints to keypoints.pcd\n");
		int size_pt = 0;
		float intensity_thresh = .0116f;
		for (size_t i = 0; i < keypoints->size(); i++)
		{
			if (keypoints->points[i].intensity >= intensity_thresh)
			{
				++size_pt;
			}
		}
		key_regions->width = size_pt;
		key_regions->height = 1;
		key_regions->is_dense = false;
		key_regions->points.resize(key_regions->width * key_regions->height);
		int kex_index = 0;
		for (size_t i = 0; i < keypoints->size(); i++)
		{
			
			if (keypoints->points[i].intensity >= intensity_thresh)
			{
				key_regions->points[kex_index].x = keypoints->points[i].x;
				key_regions->points[kex_index].y = keypoints->points[i].y;
				key_regions->points[kex_index].z = keypoints->points[i].z;
				++kex_index;
			}
		}
		pcl::io::savePCDFile("key_regions.pcd", *key_regions);

	}

	if (do_plane_segmentation == 1)
	{
		

		pcl::PCLPointCloud2::Ptr cloud_blob(new pcl::PCLPointCloud2);
		pcl::PointCloud<pcl::PointXYZ>::Ptr cloud_filtered(new pcl::PointCloud<pcl::PointXYZ>), cloud_p(new pcl::PointCloud<pcl::PointXYZ>), cloud_f(new pcl::PointCloud<pcl::PointXYZ>);

		// Fill in the cloud data
		pcl::PCDReader reader;
		reader.read("wp2_key.pcd", *cloud_blob);

		//// Convert to the templated PointCloud
		pcl::fromPCLPointCloud2(*cloud_blob, *cloud_filtered);

		std::cerr << "PointCloud after filtering: " << cloud_blob->width * cloud_blob->height << " data points." << std::endl;

		pcl::ModelCoefficients::Ptr coefficients(new pcl::ModelCoefficients());
		pcl::PointIndices::Ptr inliers(new pcl::PointIndices());
		// Create the segmentation object
		pcl::SACSegmentation<pcl::PointXYZ> seg;
		// Optional
		seg.setOptimizeCoefficients(true);
		seg.setModelType(pcl::SACMODEL_PLANE);
		seg.setMethodType(pcl::SAC_RANSAC);
		seg.setMaxIterations(10);
		seg.setDistanceThreshold(9);

		// Create the filtering object
		pcl::ExtractIndices<pcl::PointXYZ> extract;

		int i = 0, nr_points = (int)cloud_filtered->points.size();
		// While 30% of the original cloud is still there
		while (cloud_filtered->points.size() > 0.3 * nr_points)
		{
			// Segment the largest planar component from the remaining cloud
			seg.setInputCloud(cloud_filtered);
			pcl::ScopeTime scopeTime("Test loop");
			{
				seg.segment(*inliers, *coefficients);
			}
			if (inliers->indices.size() == 0)
			{
				std::cerr << "Could not estimate a planar model for the given dataset." << std::endl;
				break;
			}
			pcl::PointCloud<pcl::PointXYZ>::Ptr my_plane(new pcl::PointCloud<pcl::PointXYZ>);

			my_plane->width = inliers->indices.size();
			my_plane->height = 1;
			my_plane->is_dense = false;
			my_plane->points.resize(my_plane->width * my_plane->height);
			for (size_t i = 0; i < inliers->indices.size(); ++i)
			{
				my_plane->points[i].x = cloud_filtered->points[inliers->indices[i]].x;
				my_plane->points[i].y = cloud_filtered->points[inliers->indices[i]].y;
				my_plane->points[i].z = cloud_filtered->points[inliers->indices[i]].z;
			}
			// Extract the inliers
			extract.setInputCloud(cloud_filtered);
			extract.setIndices(inliers);
			extract.setNegative(true);
			extract.filter(*cloud_p);
			std::cerr << "PointCloud representing the planar component: " << cloud_p->width * cloud_p->height << " data points." << std::endl;
			pcl::io::savePCDFile("cloud_seg.pcd", *my_plane);
			break;
		}

	}

	if (do_smoothing == 1)
	{

		// Create a KD-Tree
		pcl::search::KdTree<pcl::PointXYZ>::Ptr tree(new pcl::search::KdTree<pcl::PointXYZ>);

		// Output has the PointNormal type in order to store the normals calculated by MLS
		pcl::PointCloud<pcl::PointNormal> mls_points;

		// Init object (second point type is for the normals, even if unused)
		pcl::MovingLeastSquares<pcl::PointXYZ, pcl::PointNormal> mls;

		mls.setComputeNormals(true);

		// Set parameters
		mls.setInputCloud(orig_pcd);
		mls.setPolynomialFit(true);
		mls.setSearchMethod(tree);
		mls.setSearchRadius(2.5);

		// Reconstruct
		mls.process(mls_points);

		// Save output
		pcl::io::savePCDFile("bun0-mls.pcd", mls_points);
	}
	
	if (do_normal_estimation == 1)
	{
		pcl::PointCloud<pcl::PointXYZ>::Ptr cloud(new pcl::PointCloud<pcl::PointXYZ>);


		pcl::NormalEstimation<pcl::PointXYZ, pcl::Normal> ne;
		ne.setInputCloud(orig_pcd);

		// Create an empty kdtree representation, and pass it to the normal estimation object.
		// Its content will be filled inside the object, based on the given input dataset (as no other search surface is given).
		pcl::search::KdTree<pcl::PointXYZ>::Ptr tree(new pcl::search::KdTree<pcl::PointXYZ>());
		ne.setSearchMethod(tree);

		// Output datasets
		pcl::PointCloud<pcl::Normal>::Ptr cloud_normals(new pcl::PointCloud<pcl::Normal>);

		// Use all neighbors in a sphere of radius 3cm
		ne.setRadiusSearch(50.0);

		// Compute the features
		ne.compute(*cloud_normals);
		pcl::PointCloud<pcl::PointNormal>::Ptr cloud_with_normals(new pcl::PointCloud<pcl::PointNormal>);

		pcl::copyPointCloud(*orig_pcd, *cloud_with_normals);
		pcl::copyPointCloud(*cloud_normals, *cloud_with_normals);

		pcl::io::savePCDFile("test.pcd", *cloud_with_normals);

		// cloud_normals->points.size () should have the same size as the input cloud->points.size ()*
	}

	if (do_sift_keypoint == 1)
	{
		const float min_scale = 0.5f;
		const int n_octaves = 1;
		const int n_scales_per_octave = 2;
		const float min_contrast = 0.001f;
		// Estimate the normals of the cloud_xyz
		pcl::NormalEstimation<pcl::PointXYZ, pcl::PointNormal> ne;
		pcl::PointCloud<pcl::PointNormal>::Ptr cloud_normals(new pcl::PointCloud<pcl::PointNormal>);
		pcl::search::KdTree<pcl::PointXYZ>::Ptr tree_n(new pcl::search::KdTree<pcl::PointXYZ>());

		ne.setInputCloud(orig_pcd);
		ne.setSearchMethod(tree_n);
		ne.setRadiusSearch(1.5);
		ne.compute(*cloud_normals);

		// Copy the xyz info from cloud_xyz and add it to cloud_normals as the xyz field in PointNormals estimation is zero
		for (size_t i = 0; i<cloud_normals->points.size(); ++i)
		{
			cloud_normals->points[i].x = orig_pcd->points[i].x;
			cloud_normals->points[i].y = orig_pcd->points[i].y;
			cloud_normals->points[i].z = orig_pcd->points[i].z;
		}

		// Estimate the sift interest points using normals values from xyz as the Intensity variants
		pcl::SIFTKeypoint<pcl::PointNormal, pcl::PointWithScale> sift;
		pcl::PointCloud<pcl::PointWithScale> result;
		pcl::search::KdTree<pcl::PointNormal>::Ptr tree(new pcl::search::KdTree<pcl::PointNormal>());
		sift.setSearchMethod(tree);
		sift.setScales(min_scale, n_octaves, n_scales_per_octave);
		sift.setMinimumContrast(min_contrast);
		sift.setInputCloud(cloud_normals);
		sift.compute(result);

		std::cout << "No of SIFT points in the result are " << result.points.size() << std::endl;

		
		// Copying the pointwithscale to pointxyz so as visualize the cloud
		pcl::PointCloud<pcl::PointXYZ>::Ptr cloud_temp (new pcl::PointCloud<pcl::PointXYZ>);
		copyPointCloud(result, *cloud_temp);
		pcl::io::savePCDFile("wp2_key.pcd", *cloud_temp);;

	}

	if (do_kdtree_search == 1)
	{
		//for cam model - 3 // cad model - 2
		pcl::io::loadPCDFile(argv[3], *orig_pcd);
		pcl::KdTreeFLANN<pcl::PointXYZ> kdtree;
		pcl::PointCloud<pcl::PointXYZ>::Ptr result_cloud(new pcl::PointCloud<pcl::PointXYZ>);
		pcl::PointCloud<pcl::PointXYZ>::Ptr after_removal(new pcl::PointCloud<pcl::PointXYZ>);
		after_removal = orig_pcd;
		ofstream myfile("example.txt");
		while (1)
		{
			if (after_removal->points.size() < 1)
			{
				cout << "limit break";
				break;
			}
			kdtree.setInputCloud(after_removal);
			pcl::PointXYZ searchPoint;
			searchPoint.x = after_removal->points[0].x;
			searchPoint.y = after_removal->points[0].y;
			searchPoint.z = after_removal->points[0].z;

			//// K nearest neighbor search

			//int K = 150;

			//std::vector<int> pointIdxNKNSearch(K);
			//std::vector<float> pointNKNSquaredDistance(K);

			//std::cout << "K nearest neighbor search at (" << searchPoint.x
			//	<< " " << searchPoint.y
			//	<< " " << searchPoint.z
			//	<< ") with K=" << K << std::endl;

			//if (kdtree.nearestKSearch(searchPoint, K, pointIdxNKNSearch, pointNKNSquaredDistance) > 0)
			//{
			//	for (size_t i = 0; i < pointIdxNKNSearch.size(); ++i)
			//		std::cout << "    " << orig_pcd->points[pointIdxNKNSearch[i]].x
			//		<< " " << orig_pcd->points[pointIdxNKNSearch[i]].y
			//		<< " " << orig_pcd->points[pointIdxNKNSearch[i]].z
			//		<< " (squared distance: " << pointNKNSquaredDistance[i] << ")" << std::endl;
			//}
			// Neighbors within radius search

			std::vector<int> pointIdxRadiusSearch;
			std::vector<float> pointRadiusSquaredDistance;

			float radius = 25.0f;
			//float radius = 7.5f;
			// cad float radius = 5.0f;
			std::cout << "Neighbors within radius search at (" << searchPoint.x
				<< " " << searchPoint.y
				<< " " << searchPoint.z
				<< ") with radius=" << radius << std::endl;


			if (kdtree.radiusSearch(searchPoint, radius, pointIdxRadiusSearch, pointRadiusSquaredDistance) > 0)
			{
				for (size_t i = 0; i < pointIdxRadiusSearch.size(); ++i)
					std::cout << "    " << after_removal->points[pointIdxRadiusSearch[i]].x
					<< " " << after_removal->points[pointIdxRadiusSearch[i]].y
					<< " " << after_removal->points[pointIdxRadiusSearch[i]].z
					<< " (squared distance: " << pointRadiusSquaredDistance[i] << ")" << std::endl;
			}

			result_cloud->width = pointIdxRadiusSearch.size();
			result_cloud->height = 1;
			result_cloud->is_dense = false;
			result_cloud->points.resize(result_cloud->width * result_cloud->height);
			pcl::PointIndices::Ptr point_indx(new pcl::PointIndices());
			if (kdtree.radiusSearch(searchPoint, radius, pointIdxRadiusSearch, pointRadiusSquaredDistance) > 0)
			{
				for (size_t i = 0; i < pointIdxRadiusSearch.size(); ++i)
				{
					result_cloud->points[i].x = after_removal->points[pointIdxRadiusSearch[i]].x;
					result_cloud->points[i].y = after_removal->points[pointIdxRadiusSearch[i]].y;
					result_cloud->points[i].z = after_removal->points[pointIdxRadiusSearch[i]].z;
					point_indx->indices.push_back(pointIdxRadiusSearch[i]);
				}
			}
			pcl::io::savePCDFile("result_pt_cloud.pcd", *result_cloud);
			pcl::ExtractIndices<pcl::PointXYZ> extract;

			pcl::PointCloud<pcl::PointXYZ>::Ptr temp_pcd(new pcl::PointCloud<pcl::PointXYZ>);

			// Extract the inliers
			extract.setInputCloud(after_removal);
			extract.setIndices(point_indx);
			extract.setNegative(true);
			extract.filter(*temp_pcd);
			after_removal = temp_pcd;
			pcl::io::savePCDFile("afterfl_pt_cloud.pcd", *after_removal);
			Eigen::Vector4f centroid;
			pcl::compute3DCentroid(*result_cloud, centroid);
			
			if (myfile.is_open())
			{
				for (size_t i = 0; i < centroid.size(); ++i)
				{
					myfile << centroid[i];
					myfile << " ";
				}
				myfile << "\n";
			}
		}
		myfile.close();
	}

	if (do_icp == 1) {


		pcl::PointCloud<pcl::PointXYZ>::Ptr cloud_source(new pcl::PointCloud<pcl::PointXYZ>);
		pcl::io::loadPCDFile(argv[6], *cloud_source);

		pcl::PointCloud<pcl::PointXYZ>::Ptr cloud_target(new pcl::PointCloud<pcl::PointXYZ>);
		pcl::io::loadPCDFile(argv[4], *cloud_target);
		pcl::IterativeClosestPoint<pcl::PointXYZ, pcl::PointXYZ> icp;
		icp.setInputSource(cloud_source);
		icp.setInputTarget(cloud_target);
		pcl::PointCloud<pcl::PointXYZ> Final;
		icp.align(Final);
		std::cout << "has converged:" << icp.hasConverged() << " score: " <<
		icp.getFitnessScore() << std::endl;
		std::cout << icp.getFinalTransformation() << std::endl;

		//write icp results
		ofstream icp_result("ICPresult.txt");
		if (icp_result.is_open())
		{

			icp_result << icp.getFinalTransformation();
		}
		icp_result.close();

	}

	return (0);
}