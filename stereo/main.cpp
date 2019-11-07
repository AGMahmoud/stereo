#include <stdlib.h>
#include <opencv2/opencv.hpp>
#include <opencv2/highgui.hpp>
#include <opencv2/imgproc.hpp>
#include "opencv2/features2d.hpp"
#include "opencv2/core.hpp"
#include <iostream>
#include <fstream>
#include <string>
#include <sstream>
#include <vector>
#include <math.h>
#include <algorithm>
#include "Features.h"
#include <Eigen/Dense>
#include <filesystem>
#include <Windows.h>
#include "Stereography.h"
#include "Estimation.h"
#include <stdlib.h>
#include <GL/glew.h> // This must appear before freeglut.h
#include <GL/freeglut.h>
#include <omp.h>

using namespace std;
using namespace cv;
using namespace Eigen;

#define STEREO_OVERLAP_THRESHOLD 20

#define BUFFER_OFFSET(offset) ((GLvoid *) offset)
#define DEBUG_FEATURES
#define DEBUG_MATCHES
//#define DEBUG_FUNDAMENTAL
#define DEBUG_ESSENTIAL_MATRIX

#define ROTATION_STEP 0.2f
#define TRANSLATION_STEP 0.1f

GLuint buffer = 0;
GLuint vPos;
GLuint program;

GLuint numPoints = 0;

/*
	This is an exercise in stereo depth-maps and reconstruction (stretch goal)
	See README for more detail

	The way this will work is for a given number of input images (start with two)
	and the associated camera calibration matrices, we'll derive the Fundamental matrices
	(although we could just do the essential matrix) between each pair of images. Once
	we have this, a depth-map for each pair of images with sufficient overlap will be computed.
	If there are enough images, we can try to do a 3D reconstruction of the scene

	Input:
	- at least two images of a scene
	- the associated camera matrices

	Output:
	- depth-map as an image

	Steps:

	Feature Detection, Scoring, Description, Matching
		Using my existing FAST Feature detecter
		FAST Features are really just not good for this. Need SIFT or something. Seriously tempted to just
		use opencv's implementation of SIFT, and save to my own format. 

	Derivation of the Fundamental Matrix 
		This implies we don't even need the camera matrices. This will be done with the normalised
		8-point algorithm by Hartley (https://en.wikipedia.org/wiki/Eight-point_algorithm#The_normalized_eight-point_algorithm)

	Triangulation
		This will be computed using Peter Lindstrom's algorithm, once I figure it out

	Depth-map


	TODO:
	- TEST NORMALISATION
	- Test triangulation
	- bring in openGL for visualisation: https://sites.google.com/site/gsucomputergraphics/educational/set-up-opengl
	- Generate a surface mesh for the point cloud
		- Read power crust
		- Hoppe's
		- Think about the bounding shape and snapping it to the mesh

   So what we're going to do is just bugger teh surface reconstruction. I'm just going to make the point clouds
   and we're going to visualise them and hopefully do SR in meshlab

	Question: why does a homography send points to points between two planes, but a fundamental matrix, 
	          still a 3x3, send a point to a line, when it is specified more?

*/
void initWithPoints(const std::vector<Vector3f>& points);
//void reshape(int width, int height);
//void display();
void reshape (int w, int h);
void renderScene(void);
void processKeys(int key, int xx, int yy);
float angle, lx, lz, x, z;
GLfloat translateX, translateY, translateZ;
GLfloat rotateX, rotateY, rotateZ;
vector<Vector3f> pointsToDraw;
// Support function
vector<string> get_all_files_names_within_folder(string folder)
{
	vector<string> names;
	string search_path = folder + "/*.*";
	WIN32_FIND_DATA fd;
	HANDLE hFind = ::FindFirstFile(search_path.c_str(), &fd);
	if (hFind != INVALID_HANDLE_VALUE) {
		do {
			// read all (real) files in current folder
			// , delete '!' read other 2 default folder . and ..
			if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
				names.push_back(fd.cFileName);
			}
		} while (::FindNextFile(hFind, &fd));
		::FindClose(hFind);
	}
	return names;
}
inline bool does_file_exist(const std::string& name) {
	ifstream f(name.c_str());
	return f.good();
}

// G

// Main
int main(int argc, char** argv)
{
	// first arg is the folder containing all the images
	if (argc < 2 || strcmp(argv[1], "-h") == 0)
	{
		cout << "Usage:" << endl;
		cout << "stereo.exe <Folder to images> <calibration file> -mask [mask image] -features [Folder to save/load features]" << endl;
		exit(1);
	}
	string featurePath = "";
	bool featureFileGiven = false;
	string pointCloudOuputPath = "";
	Mat maskImage;
	if (argc >= 3)
	{
		for (int i = 3; i < argc; i += 2)
		{
			if (strcmp(argv[i], "-mask") == 0)
			{
				maskImage = imread(argv[i+1], IMREAD_GRAYSCALE);
			}
			if (strcmp(argv[i], "-features") == 0)
			{
				featurePath = string(argv[i+1]);
				featureFileGiven = true;
			}
			if (strcmp(argv[i], "-output") == 0)
			{
				pointCloudOuputPath = string(argv[i + 1]);
			}
		}
	}

	vector<ImageDescriptor> images;
	// Create an image descriptor for each image file we have
	string imageFolder = argv[1];

	// We have the option of saving the feature descriptors out to a file
	// If we have done that, we can pull that in to avoid recomputing features every time
	bool featuresRead = false;
	if (featureFileGiven)
	{
		std::cout << "Attempting to load features from " << featurePath << std::endl;
		// If the feature file exists, read the image descriptors from it
		if (does_file_exist(featurePath))
		{
			if (ReadDescriptorsFromFile(featurePath, images))
			{
				featuresRead = true;
				cout << "Read descriptors from " << featurePath << endl;
			}
			else
			{
				std::cout << "Reading descriptors from file failed" << endl;
			}
		}
	}
	if (!featuresRead)
	{
		auto imageFiles = get_all_files_names_within_folder(imageFolder);
		for (auto& image : imageFiles)
		{
			ImageDescriptor img;
			img.filename = imageFolder + "\\" + image;
			images.push_back(img);
		}

		// Collect the camera matrices
		// Note that the camera matrices are not necessarily at the centre of their own coordinate system;
		// they may have encoded some rigid-body transform in there as well?
		vector<MatrixXf> calibrationMatrices;
		string calib = argv[2];
		ifstream calibFile;
		calibFile.open(calib);
		if (calibFile.is_open())
		{
			string line;
			while (getline(calibFile, line))
			{
				Matrix3f K;
				replace(line.begin(), line.end(), '[', ' ');
				replace(line.begin(), line.end(), ']', ' ');
				replace(line.begin(), line.end(), ';', ' ');
				// extract the numbers out of the stringstream
				vector<string> tokens;
				string token;
				stringstream stream;
				stream << line;
				while (!stream.eof())
				{
					stream >> token;
					tokens.push_back(token);
				}

				// if line contains cam
				if (tokens[0].find("cam0") != string::npos)
				{
					// Read calibration and assign to descriptor for image 0
					K << stod(tokens[1], nullptr), stod(tokens[2], nullptr), stod(tokens[3], nullptr),
						stod(tokens[4], nullptr), stod(tokens[5], nullptr), stod(tokens[6], nullptr),
						stod(tokens[7], nullptr), stod(tokens[7], nullptr), stod(tokens[9], nullptr);

					for (auto& img : images)
					{
						// Yeah, this could be a lot better, I know
						if (img.filename.find("0") != string::npos)
						{
							img.K = K / 4;
							img.K(2, 2) = 1;
							break;
						}
					}
				}
				else if (tokens[0].find("cam1") != string::npos)
				{
					// Read calibration and assign to image 1
					K << stod(tokens[1], nullptr), stod(tokens[2], nullptr), stod(tokens[3], nullptr),
						stod(tokens[4], nullptr), stod(tokens[5], nullptr), stod(tokens[6], nullptr),
						stod(tokens[7], nullptr), stod(tokens[7], nullptr), stod(tokens[9], nullptr);
					for (auto& img : images)
					{
						if (img.filename.find("1") != string::npos)
						{
							img.K = K/4;
							img.K(2, 2) = 1;
							break;
						}
					}
				}
			}
		}

		for (auto& image : images)
		{
			Mat img = imread(image.filename, IMREAD_GRAYSCALE);
			Mat mask = Mat(img.rows, img.cols, CV_8U, 255);
			
			vector<Feature> features;
			FindFASTFeatures(img, features);//FindHarrisCorners(img, 20);
			if (features.empty())
			{
				cout << "No features were found in " << image.filename << endl;
			}
			features = ScoreAndClusterFeatures(img, features);

			cout << "Found " << features.size() << " features in " << image.filename << endl;

#ifdef DEBUG_FEATURES
			Mat img_i = imread(image.filename, IMREAD_GRAYSCALE);
			for (auto& f : features)
			{
				circle(img_i, f.p, 3, (255, 255, 0), -1);
			}

			// Display
			imshow("Image - best features", img_i);
			waitKey(0);
#endif

			// Create descriptors with scale information for better matching

			// Create descriptors for each feature in the image
			std::vector<FeatureDescriptor> descriptors;
			if (!CreateSIFTDescriptors(img, features, descriptors))
			{
				cout << "Failed to create feature descriptors for image " << image.filename << endl;
				continue;
			}

			image.width = img.cols;
			image.height = img.rows;
			image.features = features;
		}
	}
	// If opted, check for a features file
	if (featureFileGiven && !featuresRead)
	{
		// If the features file does not exist, save the features out to it
		if (!does_file_exist(featurePath))
		{
			if (!SaveImageDescriptorsToFile(featurePath, images))
			{
				std::cout << "Saving descriptors to file failed" << std::endl;
			}
		}
	}

	// THis should be stored in a 2D matrix where the index in the matrix corresponds to array index
	// and the array holds the fundamental matrix
	int s = (int)images.size();
	StereoPair pair;
	cout << "Matching features for " << images[0].filename << " and " << images[1].filename << endl;
	std::vector<std::pair<Feature, Feature>> matches2 = MatchDescriptors(images[0].features, images[1].features);
	std::vector<std::pair<Feature, Feature>> matches;

	if (matches.size() < STEREO_OVERLAP_THRESHOLD)
	{
		cout << matches.size() << " features - not enough overlap between " << images[0].filename << " and " << images[1].filename << endl;
	}
	cout << matches.size() << " features found between " << images[0].filename << " and " << images[1].filename << endl;

	// normalise point coords
	/*for (auto& f : images[0].features)
	{
		f.p = Point2f(f.p.x / images[0].width, f.p.y / images[0].width);
	}
	for (auto& f : images[1].features)
	{
		f.p = Point2f(f.p.x / images[1].width, f.p.y / images[1].width);
	}
	for (auto& m : matches)
	{
		Feature& f1 = m.first;
		Feature& f2 = m.second;
		f1.p = Point2f(f1.p.x / images[0].width, f1.p.y / images[0].width);
		f2.p = Point2f(f2.p.x / images[1].width, f2.p.y / images[1].width);
	}*/


	//matches.clear();
	// Adding GT now
	Feature f1_1;
	Feature f1_2;
	f1_1.p = Point2f(88, 75);
	f1_2.p = Point2f(38, 75);
	matches.push_back(make_pair(f1_1, f1_2));
	Feature f2_1;
	Feature f2_2;
	f2_1.p = Point2f(134, 320);
	f2_2.p = Point2f(86, 320);
	matches.push_back(make_pair(f2_1, f2_2));
	Feature f3_1;
	Feature f3_2;
	f3_1.p = Point2f(107, 425);
	f3_2.p = Point2f(60, 425);
	matches.push_back(make_pair(f3_1, f3_2));
	Feature f4_1;
	Feature f4_2;
	f4_1.p = Point2f(565, 336);
	f4_2.p = Point2f(504, 336);
	matches.push_back(make_pair(f4_1, f4_2));
	Feature f5_1;
	Feature f5_2;
	f5_1.p = Point2f(643, 138);
	f5_2.p = Point2f(617, 139);
	matches.push_back(make_pair(f5_1, f5_2));
	Feature f6_1;
	Feature f6_2;
	f6_1.p = Point2f(439, 129);
	f6_2.p = Point2f(421, 129);
	matches.push_back(make_pair(f6_1, f6_2));
	Feature f7_1;
	Feature f7_2;
	f7_1.p = Point2f(268, 34);
	f7_2.p = Point2f(244, 34);
	matches.push_back(make_pair(f7_1, f7_2));
	Feature f8_1;
	Feature f8_2;
	f8_1.p = Point2f(291, 239);
	f8_2.p = Point2f(267, 239);
	matches.push_back(make_pair(f8_1, f8_2));


	// combine matches 1 and 2
	//matches.insert(matches.end(), matches2.begin(), matches2.end());
	for (auto m : matches2)
	{
		matches.push_back(m);
	}

	StereoPair stereo;
	stereo.img1 = images[0];
	stereo.img2 = images[1];

	// Compute Fundamental matrix
	Matrix3f fundamentalMatrix;
 	//if (!FindFundamentalMatrix(matches, fundamentalMatrix))
	if (!FindFundamentalMatrixWithRANSAC(matches, fundamentalMatrix, stereo))
	{
		cout << "Failed to find fundamental matrix for pair " << images[0].filename << " and " << images[1].filename << endl;
	}
	cout << "Fundamental matrix found for pair " << images[0].filename << " and " << images[1].filename << endl;


	

#ifdef DEBUG_MATCHES
	// Draw matching features
	Mat matchImageScored;
	Mat img_i = imread(images[0].filename, IMREAD_GRAYSCALE);
	Mat img_j = imread(images[1].filename, IMREAD_GRAYSCALE);
	hconcat(img_i, img_j, matchImageScored);
	int offset = img_i.cols;
	// Draw the features on the image
	for (unsigned int i = 0; i < matches.size(); ++i)
	{
		Feature f1 = matches[i].first;
		Feature f2 = matches[i].second;


		auto f = Vector3f(matches[i].first.p.x, matches[i].first.p.y, 1);
		auto fprime = Vector3f(matches[i].second.p.x, matches[i].second.p.y, 1);


		auto result = fprime.transpose() * fundamentalMatrix * f;
		std::cout << "reprojection error: " << result << endl;

		f2.p.x += offset;

		circle(matchImageScored, f1.p, 2, (255, 255, 0), -1);
		circle(matchImageScored, f2.p, 2, (255, 255, 0), -1);
		line(matchImageScored, f1.p, f2.p, (0, 0, 0), 2, 8, 0);
		
		// Debug display
		imshow("matches", matchImageScored);
		waitKey(0);
	}
	
	
#endif



	// TODO: do we need to put all features into proper image coordinates for this?


#ifdef DEBUG_FUNDAMENTAL
	// Since we use the top 8 to create the fundamental matrix, this is a good test for
	// non-matching points
	std::vector<std::pair<Feature, Feature>> temp;
	temp.insert(temp.end(), matches.begin(), matches.end());
	//matches.clear();
	for (auto& m : temp)
	{
		auto f = Vector3f(m.first.p.x, m.first.p.y, 1);
		auto fprime = Vector3f(m.second.p.x, m.second.p.y, 1);

		auto result = fprime.transpose() * fundamentalMatrix * f;
		std::cout << "reprojection error: " << result << endl;
		if (abs(result) < FUNDAMENTAL_REPROJECTION_ERROR_THRESHOLD)
		{
			matches.push_back(m);
		}
	}
	// This weeds out the bad points, leaving us with only matches that are good to triangulate
#endif
	
	
	
	
	// Compute essential matrix
	// On wikipedia, it says that E = KT * F * K
	// this makes sense, since K sends real coords into image coords
	// But in Lindstrom, it says that it assumes that points have been multiplied
	// by K inverse, and then we use E ... oh yeah duh
	// THE K's HAVE NOT BEEN SCALED YOU NUMPTY
	stereo.F = fundamentalMatrix;
	stereo.E = stereo.img2.K.transpose() * stereo.F * stereo.img1.K;

#ifdef DEBUG_ESSENTIAL_MATRIX

	// Debug the Essential Matrix now
	// We do this by drawing the epipolar line from the essential matrix at various depths,
	// and drawing the matching feature
	Vector3f t(0, 0, 0);
	Matrix3f R;
	R.setZero();
	DecomposeEssentialMatrix(stereo.E, R, t);

	// verify with the difference between t_skew * R and E
	// DEBUG
	std::cout << "Difference between E and t_skew * R:" << endl;
	Matrix3f t_skew;
	t_skew << 0, -t[2], t[1],
		t[2], 0, -t[0],
		-t[1], t[0], 0;
	Matrix3f residual = stereo.E - t_skew * R;
	cout << residual << endl;
	
	std::cout << "Rotation: \n" << R << "\nTranslation: \n" << t << endl;

	cout << "E is " << endl << stereo.E << endl << " and E inverse is " << endl << stereo.E.inverse() << endl;

	for (auto& m : matches)
	{
		Mat epipolarLines;
		Mat img_1 = imread(images[0].filename, IMREAD_GRAYSCALE);
		Mat img_2 = imread(images[1].filename, IMREAD_GRAYSCALE);
		hconcat(img_1, img_2, epipolarLines);
		int offset = img_1.cols;

		Point2f img1Point = m.first.p;// / 4;
		Point2f img2Point = m.second.p;
		Feature f2 = m.second;
		//f2.p /= 4;
		f2.p.x += offset;
		circle(epipolarLines, img1Point, 6, (255, 255, 0), -1);
		circle(epipolarLines, f2.p, 6, (255, 255, 0), -1);
		//cout << "Features are " << img1Point << " and " << f2.p << endl;

		// Here we are NOT normalising
		// But we are going from image 0 into image 1, as that is the direction in which we computed the fundamental matrix
		Vector3f projectivePoint;
		projectivePoint[0] = img1Point.x;
		projectivePoint[1] = img1Point.y;
		projectivePoint[2] = 1;
		Vector3f point = images[0].K.inverse() * projectivePoint;
		point = point / point[2];
		//cout << "Starting with " << point << endl;
		for (double d = 1; d < 10; d += 0.2)
		{
			Vector3f eL = point *d;
			//cout << "depth vector:\n" << eL << endl;
			Vector3f transformedPoint = R.inverse()* eL - R.inverse()* t; // IS THIS RIGHT?
			//cout << "transformed:\n" << transformedPoint << endl;
			transformedPoint /= transformedPoint[2];
			//cout << "normalised:\n" << transformedPoint << endl;
			// now project:
			projectivePoint = images[1].K * transformedPoint;
			// get u, v from first to bits
			Point2f reprojection(projectivePoint[0], projectivePoint[1]);
			//reprojection /= 4;
			reprojection.x += offset;
			circle(epipolarLines, reprojection, 2, (255, 255, 0), -1);
			//cout << "Epipolar line point at depth " << d << " is " << reprojection << endl;
		}

		// Display
		imshow("Epipolar line", epipolarLines);
		waitKey(0);
	}

	
#endif





	for (auto& match : matches)
	{
		Mat epipolarLines;
		Mat img_1 = imread(images[0].filename, IMREAD_GRAYSCALE);
		Mat img_2 = imread(images[1].filename, IMREAD_GRAYSCALE);
		hconcat(img_1, img_2, epipolarLines);
		int offset = img_1.cols;
		// depth is way the hell off. Fix this anohter time

		// TODO: triangulation isn't working. 
		// or it gives negative depth
		// Theory 1: mathematics is wrong somehow, not sure where
		// Can show this in OpenGL to debug easier?

		// Potential issues:
		// - coordinate normalisation?
		// - need to anti-rotate t vector? (doubt it)
		// - decomposition of E into R and t? This seems weird and too convenient

		Point2f xprime = match.first.p;
		Point2f x = match.second.p;
		Vector3f pointX = stereo.img2.K.inverse() * Vector3f(x.x, x.y, 1);
		Vector3f pointXPrime = stereo.img1.K.inverse() * Vector3f(xprime.x, xprime.y, 1);
		float d0 = 0;
		float d1 = 0;
		// TODO - do I need to do this the other way?
		// TODO - normalise or no? No?
		Matrix3f Einverse = stereo.E.inverse();
		if (!Triangulate(d0, d1, pointX, pointXPrime, stereo.E))
		{
			match.first.depth = BAD_DEPTH;
			match.second.depth = BAD_DEPTH;
			continue;
		}

		circle(epipolarLines, xprime, 3, (255, 255, 0), -1);
		x.x += offset;
		circle(epipolarLines, x, 3, (255, 255, 0), -1);

		// Test: if we can triangulate, then reproject from camera 1 to camera 0
		// and check reprojection error - this should weed out bad matches
		/*Point2f img1Point = match.second.p;
		Vector3f projectivePoint;
		projectivePoint[0] = img1Point.x*4;
		projectivePoint[1] = img1Point.y*4;
		projectivePoint[2] = 1;
		Vector3f point = images[1].K.inverse() * projectivePoint;
		point = point / point[2];
		point *= abs(d1); // TODO: fix up depth algo
		// Now apply R and t to get to cam0 frame:
		Vector3f t(0, 0, 0);
		Matrix3f R;
		R.setZero();
		DecomposeEssentialMatrix(stereo.E, R, t);
		Vector3f transformedPoint = R * point + t; // IS THIS RIGHT?
		// remove depth:
		transformedPoint /= transformedPoint[2];
		// now project:
		projectivePoint = images[0].K * transformedPoint;
		// get u, v from first to bits
		Point2f reprojection(projectivePoint[0], projectivePoint[1]);*/
		// Now compare to the original
		//cout << "Comparing " << reprojection << " to " << match.first.p.x << ", " << match.first.p.y << endl;


		// Now transform to cam 0

		// Is this depth in first frame or second frame?
		d0 = abs(d0);
		d1 = abs(d1);
		match.first.depth = abs(d0);
		match.second.depth = abs(d1);// transform the point in 3D from first camera to second camera
		cout << "Depths are " << d0 << " and " << d1 << endl;

		Vector3f t(0, 0, 0);
		Matrix3f R;
		R.setZero();
		DecomposeEssentialMatrix(stereo.E, R, t);
		Vector3f projectivePoint;
		projectivePoint[0] = xprime.x;
		projectivePoint[1] = xprime.y;
		projectivePoint[2] = 1;
		Vector3f point = images[0].K.inverse() * projectivePoint;
		point = point / point[2];
		Vector3f eL = point * d1;
		Vector3f transformedPoint = R.inverse() * eL - R.inverse() * t; // IS THIS RIGHT?
		transformedPoint /= transformedPoint[2];
		//cout << "normalised:\n" << transformedPoint << endl;
		// now project:
		projectivePoint = images[1].K * transformedPoint;
		// get u, v from first to bits
		Point2f reprojection(projectivePoint[0], projectivePoint[1]);





		// Transform points into each camera frame

		// Copy the depths to the StereoPair array
		for (auto& f : images[0].features)
		{
			if (f == match.first)
			{
				f.depth = match.first.depth;
			}
		}
		for (auto& f : images[1].features)
		{
			if (f == match.second)
			{
				//f.depth = match.second.depth;
			}
		}

		reprojection.x += offset;
		circle(epipolarLines, reprojection, 5, (255, 255, 0), 2);
		cout << "Point at depth " << d0 << " is " << reprojection << endl;

		// Display
		imshow("Depths", epipolarLines);
		waitKey(0);
	}
	

	// Render points
	pointsToDraw.clear();
	for (int i = 0; i < s; ++i)
	{
		for (auto& f : images[i].features)
		{
			if (abs(f.depth) > 1000)
			{
				// remove nonsense
				continue;
			}

			Vector3f projectivePoint;
			projectivePoint[0] = f.p.x;
			projectivePoint[1] = f.p.y;
			projectivePoint[2] = 1;
			Vector3f point = images[i].K.inverse()*projectivePoint;
			point = point / point[2];
			point *= f.depth;
			pointsToDraw.push_back(point);
		}

		// Only use the first image points
		break;
	}

	// Write these points out to a text file
	std::ofstream pointFile(pointCloudOuputPath + "\\point_cloud.txt");
	if (pointFile.is_open())
	{
		for (int i = 0; i < pointsToDraw.size(); ++i)
		{
			auto& p = pointsToDraw[i];
			// get normals
			Vector3f normal = p;
			normal *= 1 / sqrt(normal[0] * normal[0] + normal[1]*normal[1] + normal[2]*normal[2]);
			pointFile << p[0] << " " << p[1] << " " << p[2] << " " << normal[0] << " " << normal[1] << " " << normal[2];
			if (i < pointsToDraw.size() - 1)
			{
				pointFile << endl;
			}
		}
		pointFile.close();
	}

	return 0;
}




























/* ############################################################################
    OpenGL section below
   ############################################################################ */

/*
	OpenGL helpers for drawing
	I couldn't figure out how to have this in a different file, so it's all here
*/

void initWithPoints(const std::vector<Vector3f>& points)
{
	GLfloat* vertices = (GLfloat*)malloc(sizeof(GLfloat*)*points.size()*4);
	for (size_t i = 0; i < points.size(); ++i)
	{
		vertices[4*i] = points[i][0];
		vertices[4*i+1] = points[i][1];
		vertices[4*i+2] = points[i][2];
		vertices[4*i+3] = 1;
	}
	numPoints = (GLuint)points.size();
	angle = 0;
	lx = 0;
	lz = 0;
	x = 0;
	z = 0;


	// Get an unused buffer object name. Required after OpenGL 3.1. 
	glGenBuffers(1, &buffer);

	// If it's the first time the buffer object name is used, create that buffer. 
	glBindBuffer(GL_ARRAY_BUFFER, buffer);

	// Allocate memory for the active buffer object. 
	// 1. Allocate memory on the graphics card for the amount specified by the 2nd parameter.
	// 2. Copy the data referenced by the third parameter (a pointer) from the main memory to the 
	//    memory on the graphics card. 
	// 3. If you want to dynamically load the data, then set the third parameter to be NULL. 
	glBufferData(GL_ARRAY_BUFFER, sizeof(GLfloat)*4*points.size(), vertices, GL_DYNAMIC_DRAW);

	// OpenGL vertex shader source code
	const char* vSource = {
		"#version 330\n"
		"in vec4 vPos;"
		"void main() {"
		"	gl_Position = vPos * vec4(1.0f, 1.0f, 1.0f, 1.0f);"
		"}"
	};

	// OpenGL fragment shader source code
	const char* fSource = {
		"#version 330\n"
		"out vec4 fragColor;"
		"void main() {"
		"	fragColor = vec4(0.8, 0.8, 0, 1);"
		"}"
	};

	// Declare shader IDs
	GLuint vShader, fShader;

	// Create empty shader objects
	vShader = glCreateShader(GL_VERTEX_SHADER);
	fShader = glCreateShader(GL_FRAGMENT_SHADER);

	// Attach shader source code the shader objects
	glShaderSource(vShader, 1, &vSource, NULL);
	glShaderSource(fShader, 1, &fSource, NULL);

	// Compile shader objects
	glCompileShader(vShader);
	glCompileShader(fShader);

	// Create an empty shader program object
	program = glCreateProgram();

	// Attach vertex and fragment shaders to the shader program
	glAttachShader(program, vShader);
	glAttachShader(program, fShader);

	// Link the shader program
	glLinkProgram(program);

	// Retrieve the ID of a vertex attribute, i.e. position
	vPos = glGetAttribLocation(program, "vPos");

	// Specify the background color
	glClearColor(0, 0, 0, 1);
}
/*
void reshape(int width, int height)
{
	// Compute aspect ratio of the new window
	if (height == 0) height = 1;                // To prevent divide by 0
	GLfloat aspect = (GLfloat)width / (GLfloat)height;

	// Set the viewport to cover the new window
	glViewport(0, 0, width, height);

	// Set the aspect ratio of the clipping volume to match the viewport
	glMatrixMode(GL_PROJECTION);  // To operate on the Projection matrix
	glLoadIdentity();             // Reset
	// Enable perspective projection with fovy, aspect, zNear and zFar
	gluPerspective(45.0f, aspect, 0.1f, 100.0f);
}

void display()
{
	// Clear the window with the background color
	glClear(GL_COLOR_BUFFER_BIT);

	glMatrixMode(GL_MODELVIEW); //set the matrix to model view mode

    glPushMatrix(); // push the matrix

	// Activate the shader program
	glUseProgram(program);

	// If the buffer object already exists, make that buffer the current active one. 
	// If the buffer object name is 0, disable buffer objects. 
	glBindBuffer(GL_ARRAY_BUFFER, buffer);

	// Associate the vertex array in the buffer object with the vertex attribute: "position"
	glVertexAttribPointer(vPos, 4, GL_FLOAT, GL_FALSE, 0, BUFFER_OFFSET(0));

	// Enable the vertex attribute: "position"
	glEnableVertexAttribArray(vPos);

	// Start the shader program
	glDrawArrays(GL_POINTS, 0, numPoints);

	// Are these necessary?
	glDisableVertexAttribArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

	glPopMatrix();//pop the matrix

    glMatrixMode(GL_PROJECTION); // Apply projection matrix again

	// Refresh the window
	glutSwapBuffers();
}*/

void processKeys(int key, int xx, int yy)
{
	switch (key) {
		case GLUT_KEY_LEFT :
			rotateY -= ROTATION_STEP;
			break;
		case GLUT_KEY_RIGHT :
			rotateY += ROTATION_STEP;
			break;
		case GLUT_KEY_UP :
			rotateX += ROTATION_STEP;
			break;
		case GLUT_KEY_DOWN :
			rotateX -= ROTATION_STEP;
			break;
		case GLUT_KEY_CTRL_L:
			translateX += TRANSLATION_STEP;
			break;
		case GLUT_KEY_CTRL_R:
			translateX -= TRANSLATION_STEP;
			break;
		case GLUT_KEY_ALT_L:
			translateY += TRANSLATION_STEP;
			break;
		case GLUT_KEY_ALT_R:
			translateY -= TRANSLATION_STEP;
			break;
		case GLUT_KEY_SHIFT_L:
			translateZ += TRANSLATION_STEP;
			break;
		case GLUT_KEY_SHIFT_R:
			translateZ -= TRANSLATION_STEP;
			break;
		case GLUT_KEY_END :
			// reset all values
			translateX = 0.f;
			translateY = 0.f;
			translateZ = 0.f;
			rotateX = 0.f;
			rotateY = 0.f;
			rotateZ = 0.f;
			break;
	}
}

void reshape (int w, int h) 
{
	glViewport (0, 0, (GLsizei)w, (GLsizei)h);  
	glMatrixMode (GL_PROJECTION);  
	glLoadIdentity ();  
	gluPerspective (60, (GLfloat)w / (GLfloat)h, 1.0, 500.0);
	glMatrixMode (GL_MODELVIEW);  
}

void renderScene(void)
{
	glClear (GL_COLOR_BUFFER_BIT);  
	glLoadIdentity();
	// Now update transformation matrices with keys

	// translation
	glTranslatef(translateX, translateY, translateZ);

	// rotation
	glRotatef(rotateX, 1.0, 0.0, 0.0); //rotate about the x axis
	glRotatef(rotateY, 0.0, 1.0, 0.0); //rotate about the y axis
	glRotatef(rotateZ, 0.0, 0.0, 1.0); //rotate about the z axis

	


	float x,y,z;  
	glPointSize(2.0);   
	glBegin(GL_POINTS);  
	for (const auto& p : pointsToDraw)
	{
		glVertex3f(p[0],p[1],p[2]);  
	}

	glEnd();  
	glFlush();  

	glutSwapBuffers();
}