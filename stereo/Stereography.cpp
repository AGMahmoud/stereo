#include "Stereography.h"
#include <stdlib.h>
#include <iostream>
#include <algorithm>

using namespace cv;
using namespace std;
using namespace Eigen;

/*
	Find the Fundamental Matrix between two images, assuming they have enough overlap, 
	given their matching features. 

	This implements the normalised 8-point algorithm. 
	The algorithm is essentially:
	- normalise all points so that the centre of all points is the origin, and the
	  points are scaled so that the average distance to the origin is root 2
	- get the top 8 points (or more) and do SVD to get the homography between them
	  subject to the epipolar constraint
	- denormalise

	The normalisation reduces numerical error when some values are large and others are small
	and yet they are all compared together and use the same error. This basically translates
	to over-error or under-error for points. 
*/
// Support functions
void GetNormalisationTransformAndNormalisePoints(vector<pair<Feature, Feature>> matches, Matrix3f& T1, Matrix3f& T2)
{
	// Get centroid of points for each image
	Point2f centroid1(0,0);
	Point2f centroid2(0, 0);
	for (auto& m : matches)
	{
		centroid1 += m.first.p;
		centroid2 += m.second.p;
	}
	centroid1 /= (float)matches.size();
	centroid2 /= (float)matches.size();

	// offset all points by the negative centroid
	for (auto& m : matches)
	{
		m.first.p -= centroid1;
		m.second.p -= centroid2;
	}

	// Find the average distance to the centre
	float avgDist1 = 0;
	float avgDist2 = 0;
	for (auto& m : matches)
	{
		avgDist1 += sqrt(m.first.p.dot(m.first.p));
		avgDist2 += sqrt(m.second.p.dot(m.second.p));
	}
	avgDist1 /= (float)matches.size();
	avgDist2 /= (float)matches.size();

	// Now scale every point by root 2 over this distance
	float scale1 = sqrt(2) / avgDist1;
	float scale2 = sqrt(2) / avgDist2;
	for (auto& m : matches)
	{
		m.first.p *= scale1;
		m.second.p *= scale2;
	}

	T1 << scale1,   0,    -1*centroid1.x*scale1,
		    0,    scale1, -1*centroid1.y*scale1,
		    0,      0,        1;

	T2 << scale2,   0,    -1*centroid2.x*scale2,
		    0,    scale2, -1*centroid2.y*scale2,
		    0,      0,        1;
}
// Actual function
bool FindFundamentalMatrix(const vector<pair<Feature, Feature>>& matches, Matrix3f& F)
{
	if (matches.size() < 8)
	{
		return false;
	}

	// create a local copy
	vector<pair<Feature, Feature>> pairs;
	for (auto m : matches)
	{
		pairs.push_back(m);
	}

	// Normalise points and get the transforms for denormalisation
	Matrix3f normalise1, normalise2;
	normalise1.setZero();
	normalise2.setZero();
	GetNormalisationTransformAndNormalisePoints(pairs, normalise1, normalise2);

	// apply the normalisation to the features
	vector< pair<Vector3f, Vector3f>> vectorPairs;
	for (auto& m : pairs)
	{
		Vector3f first = Vector3f(m.first.p.x, m.first.p.y, 1);
		Vector3f second = Vector3f(m.second.p.x, m.second.p.y, 1);

		first = normalise1 * first;
		second = normalise2 * second;

		vectorPairs.push_back(make_pair(first, second));
	}

	// Select some subset - here all - and form a system of linear equations based on the 
	// epipolar constraint
	// In theory, it shouldn't matter which 8 we pick
	// also in theory, we have a strong matching set of points - why not use all?
	// We should just use the first 8
	MatrixXf Y;
	Y.resize(vectorPairs.size(), 9);
	// The matrix Y follows the constraint of y' E y = 0 where y' is from the second feature
	// and y is from the first
	for (int i = 0; i < vectorPairs.size(); ++i)
	{
		Point2f y = Point2f(vectorPairs[i].first[0], vectorPairs[i].first[1]);
		Point2f yprime = Point2f(vectorPairs[i].second[0], vectorPairs[i].second[1]);
		Y(i, 0) = yprime.x * y.x;
		Y(i, 1) = yprime.x * y.y;
		Y(i, 2) = yprime.x;
		Y(i, 3) = yprime.y * y.x;
		Y(i, 4) = yprime.y * y.y;
		Y(i, 5) = yprime.y;
		Y(i, 6) = y.x;
		Y(i, 7) = y.y;
		Y(i, 8) = 1;
	}

	// Solve with SVD
	BDCSVD<MatrixXf> svd(Y, ComputeFullU | ComputeFullV);
	if (!svd.computeV())
		return false;
	auto & f = svd.matrixV();
	auto& d = svd.singularValues();

	// Does this have any constraints on the singular values?
	// Two things:
	// - We can enforce the rank 2 constraint
	// - we can make the f vector have norm 1
	VectorXf fprime(9);
	fprime << f(0, 8), f(1, 8), f(2, 8),
		f(3, 8), f(4, 8), f(5, 8),
		f(6, 8), f(7, 8), f(8, 8);
	fprime.normalize();

	Matrix3f normalisedF;
	normalisedF << fprime(0), fprime(1), fprime(2),
		fprime(3), fprime(4), fprime(5),
		fprime(6), fprime(7), fprime(8);

	// Transform the matrix back to the original coordinate system
	F = normalise2.transpose() * normalisedF * normalise1;
	F /= F(2, 2);

	return true;
}

float ReprojectionError(Matrix3f E, Matrix3f R, Vector3f t, Matrix3f K1, Matrix3f K2, Vector3f p1, Vector3f p2)
{
	float error = 0;

	Vector3f point1 = K1.inverse() * p1;
	Vector3f point2 = K2.inverse() * p2;
	float d0, d1;
	if (!Triangulate(d0, d1, point1, point2, E))
	{
		return 1000;
	}

	Vector3f transformedPoint = R.inverse() * (point1 * d1) - R.inverse() * t;
	transformedPoint /= transformedPoint[2];
	transformedPoint = K2 * transformedPoint;

	error = (transformedPoint - p1).norm();

	return error;
}

bool FindFundamentalMatrixWithRANSAC(const vector<pair<Feature, Feature>>& matches, Matrix3f& F, StereoPair& stereo)
{
	// For a number of iterations
	// pick a random 8 points
	// Check the reprojection error by computing x' * F * x - this should be close to zero
	// The F with the most inliers wins
	Matrix3f currentBestFundamentalMatrix;
	int minError = FUNDAMENTAL_REPROJECTION_ERROR_THRESHOLD* MIN_NUM_INLIERS;
	int iterations = 0;
	srand(F(0,0));

	// copy the set of points
	vector<pair<Feature, Feature>> pairs;

	do
	{
		pairs.clear();
		pairs.insert(pairs.end(), matches.begin(), matches.end());
		vector<pair<Feature, Feature>> chosenEight;

		// pick 8 random
		while (chosenEight.size() < 8)
		{
			int randNum = rand() % pairs.size();
			auto featurePair = pairs[randNum];
			chosenEight.push_back(featurePair);
			pairs.erase(pairs.begin() + randNum);
		}

		Matrix3f fundamental;
		if (FindFundamentalMatrix(chosenEight, fundamental))
		{
			// Now find reprojection error of points
			int localInliers = 0;
			float avgError = 0;

			Matrix3f E = stereo.img2.K.transpose() * fundamental * stereo.img1.K;
			Matrix3f R;
			R.setZero();
			Vector3f t(0, 0, 0);
			DecomposeEssentialMatrix(E, R, t);
			for (auto& m : pairs)
			{
				auto f = Vector3f(m.first.p.x, m.first.p.y, 1);
				auto fprime = Vector3f(m.second.p.x, m.second.p.y, 1);

				float reprojectionError = ReprojectionError(E, R, t, stereo.img1.K, stereo.img2.K, f, fprime);
				if (reprojectionError < FUNDAMENTAL_REPROJECTION_ERROR_THRESHOLD)
				{
					localInliers++;
					avgError += reprojectionError;
				}
				
			}
			if (localInliers > 0)
				avgError /= localInliers;
			cout << avgError << endl;
			if (localInliers > MIN_NUM_INLIERS && avgError < minError)
			{
				minError = avgError;
				currentBestFundamentalMatrix = fundamental;

#ifdef DEBUG_RANSAC_FUNDAMENTAL
				cout << "Best one so far is " << localInliers << " inliers with average distance " << avgError << endl;

				// visualise which 8 we picked from
				//Mat funamental(476, 699, CV_8U, Scalar(0));
				Mat matchImageScored;
				Mat img_i(476, 699, CV_8U, Scalar(127));
				Mat img_j(476, 699, CV_8U, Scalar(127));
				hconcat(img_i, img_j, matchImageScored);
				int offset = img_i.cols;
				for (auto p : chosenEight)
				{
					auto f2 = p.second;
					f2.p.x += offset;
					circle(matchImageScored, p.first.p, 4, 255, -1);
					circle(matchImageScored, f2.p, 4, 255, -1);
					line(matchImageScored, p.first.p, f2.p, (0, 0, 0), 2, 8, 0);
				}
				imshow("fundamental", matchImageScored);
				waitKey(0);
#endif
			}
		}

		iterations++;
		//cout << "iteration " << iterations << endl;
	} while (iterations < FUNDAMENTAL_RANSAC_ITERATIONS);
	
	if (minError < FUNDAMENTAL_REPROJECTION_ERROR_THRESHOLD * MIN_NUM_INLIERS)
	{
		F = currentBestFundamentalMatrix;
		return true;
	}
	return false;
}

/*
	Triangulate using Peter Lindstrom's algorithm
	https://e-reports-ext.llnl.gov/pdf/384387.pdf
	We use algorithm niter1 in Listing 3

	If we want to use this with no scaling, then we must use the Essential matrix.
	Once we have the camera matrix per image, this is easy. 

	To do this with the Fundamental matrix, we replace E with F in the algorithm, but
	for the Euclidean reprojection error to make sense pixels must be square. So we have to scale. 

	First we optimise with Lindstrom's algorithm, and then we use the naive depth computation. 

	I call this optimisation, because it's not really triangulation; all it does
	is improve x and x' for actually getting the depth. The depth-getting algorithm comes next.

*/
// Helpers
bool DecomposeEssentialMatrix(Matrix3f& E, Matrix3f& R, Vector3f& t)
{
	BDCSVD<MatrixXf> svd_initial(E, ComputeFullU | ComputeFullV);
	if (!svd_initial.computeV())
		return false;
	if (!svd_initial.computeU())
		return false;
	auto& d = svd_initial.singularValues();

	// To ensure that we have singular values of 1 1 0, 
	// we scale E
	// let the first singular factor be the scalar
	float scalar = d(0);
	E /= scalar;

	BDCSVD<MatrixXf> svd(E, ComputeFullU | ComputeFullV);
	if (!svd.computeV())
		return false;
	if (!svd.computeU())
		return false;
	auto& v = svd.matrixV();
	auto& u = svd.matrixU();

	MatrixXf V = v.transpose().transpose();
	MatrixXf U = u.transpose().transpose();
	if ((u * V.transpose()).determinant() == -1)
	{
		V *= -1.f;
	}

	Matrix3f W;
	W << 0, -1, 0,
		 1,  0, 0,
		 0,  0, 1;

	R = u * W * V.transpose();
	// Or R could also be
	R = U * W.transpose() * V.transpose();
	// this second one is right somehow ... Basically the way to check is on the 3D points
	t(0) = U(0,2);
	t(1) = U(1, 2);
	t(2) = U(2, 2);

	// we either need t or -t

	// t can also be t = UWDU.transpose()
	// or t = UZU.transpose, z = -W without the 1 in the borrom right

	return true;
}
void LindstromOptimisation(Vector3f& x, Vector3f& xprime, const Matrix3f E)
{
	MatrixXf S(2, 3);
	S << 1, 0, 0,
		0, 1, 0;

	Matrix2f Etilde = S * E * S.transpose();

	Vector2f n = S * E * xprime;
	Vector2f nprime = S * E.transpose() * x;
	RowVector2f nT = n.transpose();
	RowVector2f nprimeT = nprime.transpose();
	float a = nT * Etilde * nprime;
	float b = 0.5f * (nT*n + nprimeT*nprime)(0);
	float c = x.transpose() * E * xprime;
	float d = sqrt(b*b - a*c);
	float lambda = c / (b + d);
	Vector2f x_delta = lambda * n;
	Vector2f xprime_delta = lambda * nprime;
	n = n - Etilde * xprime_delta;
	nT = n.transpose();
	nprime = nprime - Etilde * x_delta;
	nprimeT = nprime.transpose();
	x_delta = ((x_delta.transpose()*n)(0) / (nT*n)(0)) * n;
	xprime_delta = ((xprime_delta.transpose()*nprime)(0) / (nprimeT*nprime)(0)) * nprime;
	x = x - S.transpose() * x_delta;
	xprime = xprime - S.transpose() * xprime_delta;
}
// Actual Function
bool Triangulate(float& depth0, float& depth1, Vector3f& x, Vector3f& xprime, Matrix3f& E)
{
	// Lindstrom's algorithm gives us the optimal points x and xprime
	// So we modify p1 and p2, and then use them to compute depth. 
	LindstromOptimisation(x, xprime, E);

	// Now that we have the very best points we can get, we use the naive depth-getter
	// This basically shoots a ray out from each point, and draws a line between the two rays
	// and finds the point on each ray that minimises the length of this line. 
	// Basically the most agreeable point. The depth along each ray, then, is the point depth. 

	Vector3f t(0, 0, 0);
	Matrix3f R;
	R.setZero();
	if (!DecomposeEssentialMatrix(E, R, t))
	{
		return false;
	}

	Vector3f normalisedX = x / x(2);
	Vector3f normalisedXPrime = xprime / xprime(2);
	Vector3f u = R * normalisedX;
	Vector3f v = normalisedXPrime;

	u = u / u.norm();
	v = v / v.norm();

	float a = u.dot(t);
	float b = u.dot(u);
	float c = u.dot(v);
	float d = v.dot(t);
	float e = v.dot(v);

	float g = c * c - b * e;
	if (fabs(g) < 1e-9) return false;
	//{
		// do this the other way
		// turns out this doesn't work. 
		// This gets degenerate solutions when the epipolar lines are parallel
		// Need to compute an example with some rotation 
	//}
	float d0 = 0;
	float d1 = 0;
	if (fabs(c) < 1e-9)// return false;
	{
		d1 = (a * c - b * d) / (c * c - b * e);
		d0 = (c * d1 - a) / b;
	}
	else
	{
		d0 = (a * e - c * d) / (c * c - b * e);
		d1 = (a + b * d0) / c;
	}

	Vector3f xyz0 = t + d0 * u;
	Vector3f xyz1 = d1 * v;

	Vector3f midpoint = 0.5 * (xyz0 + xyz1);
	Vector3f point3D = E.inverse() * midpoint;
	depth0 = d0;
	depth1 = d1;
	// It's worth noting that we compute the final 3d point, and the distance in both cameras

	return true;
}

/*
	Given a projective matrix P, decompose into K and E = R * t_skew
	
	The calibration matrices that we get from this dataset come as full 3x4 projective matrices
	in the form P = [A|-AC], where M is 3x3 and -AC is the translation.
	
	So we need to decompose this into K, R and t. Then the essential matrix is just R * t_skew
	t is simple - get the last column, multiply by -M. Bam done.
	
	For K and R, we know that K is an upper triangular matrix, up to a scale factor, and R is orthogonal
	by virtue of being a rotation matrix. Conveniently, RQ decomposition decomposes a matrix A into 
	two components, one being upper-triangular - R - and the other being orthogonal. While every rotation
	is orthogonal, it's also true that every orthogonal matrix is a rotation. 

	Questions:
	- is this how the translation is computed? What if it is just the final column?
	- Do we need to scale K? Divide by final element?
*/
void DecomposeProjectiveMatrixIntoKAndE(const MatrixXf& P, Matrix3f& K, Matrix3f& E)
{
	Matrix3f A = P.block<3, 3>(0,0);
	Vector3f t(P(0,3), P(1,3), P(2,3));
	t = -1 * A * t;
	
	// Perform RQ decomposition - See Appendix 4 of Multiple View Geometry for details, section A4.1.1
	// We left-multiply by three Givens rotations, the angle of each we need to derive. 

	// First we zero A_32 by multiplying by the x Givens rotation
	float fraction = 1 / sqrt(A(2,1)*A(2,1) + A(2,2)*A(2,2));
	Matrix3f Qx;
	Qx << 1,        0,               0,
		  0, -A(2,2)*fraction, -A(2,1)*fraction,
		  0,  A(2,1)*fraction, -A(2,2)*fraction;
	A = A * Qx;

	// Now zero A_31 using the y Givens rotation
	fraction = 1 / sqrt(A(2, 0) * A(2, 0) + A(2, 2) * A(2, 2));
	Matrix3f Qy;
	Qy << A(2, 2) * fraction, 0, A(2, 0) * fraction,
		          0,          1,         0,
		 -A(2, 0) * fraction, 0, A(2, 2) * fraction;
	A = A * Qy;

	// Now zero A_21 using Qz
	fraction = 1 / sqrt(A(1, 1) * A(1, 1) + A(1, 0) * A(1, 0));
	Matrix3f Qz;
	Qz << -A(1, 1) * fraction, -A(1, 0) * fraction, 0,
		   A(1, 0) * fraction, -A(1, 1) * fraction, 0,
		           0,                   0,          1;
	A = A * Qz;

	// We had A = some blend of K and R. Now we left-multiply to get
	// AQxQyQz = some upper triangular matrix, which must be K up to a scale factor
	// Therefore, (QxQyQz).transpose() = R, our rotation
	K = A;
	K /= K(2, 2);
	Matrix3f R = (Qx * Qy * Qz).transpose();

	// To get E, we do R * t_skew
	Matrix3f t_skew;
	t_skew << 0,   -t[2],  t[1],
		     t[2],   0,   -t[0],
		    -t[1],  t[0],   0;
	E = R * t_skew;
}

/*
	Read calibration matrices from given files, 
	using the specific format of the middlebury dataset
*/
void ReadCalibrationMatricesFromFile(
	_In_ const std::string& calibFilename,
	_Inout_ std::vector<ImageDescriptor>& images)
{
	ifstream calibFile;
	calibFile.open(calibFilename);
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
						img.K = K / 4;
						img.K(2, 2) = 1;
						break;
					}
				}
			}
		}
	}
}