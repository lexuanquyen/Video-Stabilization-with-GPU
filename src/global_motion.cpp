/*M///////////////////////////////////////////////////////////////////////////////////////
//
//  IMPORTANT: READ BEFORE DOWNLOADING, COPYING, INSTALLING OR USING.
//
//  By downloading, copying, installing or using the software you agree to this license.
//  If you do not agree to this license, do not download, install,
//  copy or use the software.
//
//
//                           License Agreement
//                For Open Source Computer Vision Library
//
// Copyright (C) 2000-2008, Intel Corporation, all rights reserved.
// Copyright (C) 2009-2011, Willow Garage Inc., all rights reserved.
// Third party copyrights are property of their respective owners.
//
// Redistribution and use in source and binary forms, with or without modification,
// are permitted provided that the following conditions are met:
//
//   * Redistribution's of source code must retain the above copyright notice,
//     this list of conditions and the following disclaimer.
//
//   * Redistribution's in binary form must reproduce the above copyright notice,
//     this list of conditions and the following disclaimer in the documentation
//     and/or other materials provided with the distribution.
//
//   * The name of the copyright holders may not be used to endorse or promote products
//     derived from this software without specific prior written permission.
//
// This software is provided by the copyright holders and contributors "as is" and
// any express or implied warranties, including, but not limited to, the implied
// warranties of merchantability and fitness for a particular purpose are disclaimed.
// In no event shall the Intel Corporation or contributors be liable for any direct,
// indirect, incidental, special, exemplary, or consequential damages
// (including, but not limited to, procurement of substitute goods or services;
// loss of use, data, or profits; or business interruption) however caused
// and on any theory of liability, whether in contract, strict liability,
// or tort (including negligence or otherwise) arising in any way out of
// the use of this software, even if advised of the possibility of such damage.
//
//M*/

#include "../include/precomp.hpp"
#include "../include/global_motion.hpp"
#include "../include/ring_buffer.hpp"
#include "../include/outlier_rejection.hpp"
#include "opencv2/opencv_modules.hpp"
#include "../include/clp.hpp"
#include "../include/define.hpp"
#include "opencv2/core/ocl.hpp"
//#include <CL/cl.h>
using namespace std;
namespace cv
{
namespace videostab
{
	
// does isotropic normalization
static Mat normalizePoints(int npoints, Point2f *points)
{
    float cx = 0.f, cy = 0.f;
    for (int i = 0; i < npoints; ++i)
    {
        cx += points[i].x;
        cy += points[i].y;
    }
    cx /= npoints;
    cy /= npoints;

    float d = 0.f;
    for (int i = 0; i < npoints; ++i)
    {
        points[i].x -= cx;
        points[i].y -= cy;
        d += sqrt(sqr(points[i].x) + sqr(points[i].y));
    }
    d /= npoints;

    float s = sqrt(2.f) / d;
    for (int i = 0; i < npoints; ++i)
    {
        points[i].x *= s;
        points[i].y *= s;
    }

    Mat_<float> T = Mat::eye(3, 3, CV_32F);
    T(0,0) = T(1,1) = s;
    T(0,2) = -cx*s;
    T(1,2) = -cy*s;
    return T;
}


static Mat estimateGlobMotionLeastSquaresTranslation(
        int npoints, Point2f *points0, Point2f *points1, float *rmse)
{
    Mat_<float> M = Mat::eye(3, 3, CV_32F);
    for (int i = 0; i < npoints; ++i)
    {
        M(0,2) += points1[i].x - points0[i].x;
        M(1,2) += points1[i].y - points0[i].y;
    }
    M(0,2) /= npoints;
    M(1,2) /= npoints;

    if (rmse)
    {
        *rmse = 0;
        for (int i = 0; i < npoints; ++i)
            *rmse += sqr(points1[i].x - points0[i].x - M(0,2)) +
                     sqr(points1[i].y - points0[i].y - M(1,2));
        *rmse = sqrt(*rmse / npoints);
    }

    return M;
}


static Mat estimateGlobMotionLeastSquaresTranslationAndScale(
        int npoints, Point2f *points0, Point2f *points1, float *rmse)
{
    Mat_<float> T0 = normalizePoints(npoints, points0);
    Mat_<float> T1 = normalizePoints(npoints, points1);

    Mat_<float> A(2*npoints, 3), b(2*npoints, 1);
    float *a0, *a1;
    Point2f p0, p1;

    for (int i = 0; i < npoints; ++i)
    {
        a0 = A[2*i];
        a1 = A[2*i+1];
        p0 = points0[i];
        p1 = points1[i];
        a0[0] = p0.x; a0[1] = 1; a0[2] = 0;
        a1[0] = p0.y; a1[1] = 0; a1[2] = 1;
        b(2*i,0) = p1.x;
        b(2*i+1,0) = p1.y;
    }

    Mat_<float> sol;
    solve(A, b, sol, DECOMP_NORMAL | DECOMP_LU);

    if (rmse)
        *rmse = static_cast<float>(norm(A*sol, b, NORM_L2) / sqrt(static_cast<double>(npoints)));

    Mat_<float> M = Mat::eye(3, 3, CV_32F);
    M(0,0) = M(1,1) = sol(0,0);
    M(0,2) = sol(1,0);
    M(1,2) = sol(2,0);

    return T1.inv() * M * T0;
}

static Mat estimateGlobMotionLeastSquaresRotation(
        int npoints, Point2f *points0, Point2f *points1, float *rmse)
{
    Point2f p0, p1;
    float A(0), B(0);
    for(int i=0; i<npoints; ++i)
    {
        p0 = points0[i];
        p1 = points1[i];

        A += p0.x*p1.x + p0.y*p1.y;
        B += p0.x*p1.y - p1.x*p0.y;
    }

    // A*sin(alpha) + B*cos(alpha) = 0
    float C = sqrt(A*A + B*B);
    Mat_<float> M = Mat::eye(3, 3, CV_32F);
    if ( C != 0 )
    {
        float sinAlpha = - B / C;
        float cosAlpha = A / C;

        M(0,0) = cosAlpha;
        M(1,1) = M(0,0);
        M(0,1) = sinAlpha;
        M(1,0) = - M(0,1);
    }

    if (rmse)
    {
        *rmse = 0;
        for (int i = 0; i < npoints; ++i)
        {
            p0 = points0[i];
            p1 = points1[i];
            *rmse += sqr(p1.x - M(0,0)*p0.x - M(0,1)*p0.y) +
                     sqr(p1.y - M(1,0)*p0.x - M(1,1)*p0.y);
        }
        *rmse = sqrt(*rmse / npoints);
    }

    return M;
}

static Mat  estimateGlobMotionLeastSquaresRigid(
        int npoints, Point2f *points0, Point2f *points1, float *rmse)
{
    Point2f mean0(0.f, 0.f);
    Point2f mean1(0.f, 0.f);

    for (int i = 0; i < npoints; ++i)
    {
        mean0 += points0[i];
        mean1 += points1[i];
    }

    mean0 *= 1.f / npoints;
    mean1 *= 1.f / npoints;

    Mat_<float> A = Mat::zeros(2, 2, CV_32F);
    Point2f pt0, pt1;

    for (int i = 0; i < npoints; ++i)
    {
        pt0 = points0[i] - mean0;
        pt1 = points1[i] - mean1;
        A(0,0) += pt1.x * pt0.x;
        A(0,1) += pt1.x * pt0.y;
        A(1,0) += pt1.y * pt0.x;
        A(1,1) += pt1.y * pt0.y;
    }

    Mat_<float> M = Mat::eye(3, 3, CV_32F);

    SVD svd(A);
    Mat_<float> R = svd.u * svd.vt;
    Mat tmp(M(Rect(0,0,2,2)));
    R.copyTo(tmp);

    M(0,2) = mean1.x - R(0,0)*mean0.x - R(0,1)*mean0.y;
    M(1,2) = mean1.y - R(1,0)*mean0.x - R(1,1)*mean0.y;

    if (rmse)
    {
        *rmse = 0;
        for (int i = 0; i < npoints; ++i)
        {
            pt0 = points0[i];
            pt1 = points1[i];
            *rmse += sqr(pt1.x - M(0,0)*pt0.x - M(0,1)*pt0.y - M(0,2)) +
                     sqr(pt1.y - M(1,0)*pt0.x - M(1,1)*pt0.y - M(1,2));
        }
        *rmse = sqrt(*rmse / npoints);
    }

    return M;
}


static Mat estimateGlobMotionLeastSquaresSimilarity(
        int npoints, Point2f *points0, Point2f *points1, float *rmse)
{
    Mat_<float> T0 = normalizePoints(npoints, points0);
    Mat_<float> T1 = normalizePoints(npoints, points1);

    Mat_<float> A(2*npoints, 4), b(2*npoints, 1);
    float *a0, *a1;
    Point2f p0, p1;

    for (int i = 0; i < npoints; ++i)
    {
        a0 = A[2*i];
        a1 = A[2*i+1];
        p0 = points0[i];
        p1 = points1[i];
        a0[0] = p0.x; a0[1] = p0.y; a0[2] = 1; a0[3] = 0;
        a1[0] = p0.y; a1[1] = -p0.x; a1[2] = 0; a1[3] = 1;
        b(2*i,0) = p1.x;
        b(2*i+1,0) = p1.y;
    }

    Mat_<float> sol;
    solve(A, b, sol, DECOMP_NORMAL | DECOMP_LU);

    if (rmse)
        *rmse = static_cast<float>(norm(A*sol, b, NORM_L2) / sqrt(static_cast<double>(npoints)));

    Mat_<float> M = Mat::eye(3, 3, CV_32F);
    M(0,0) = M(1,1) = sol(0,0);
    M(0,1) = sol(1,0);
    M(1,0) = -sol(1,0);
    M(0,2) = sol(2,0);
    M(1,2) = sol(3,0);

    return T1.inv() * M * T0;
}


static Mat estimateGlobMotionLeastSquaresAffine(
        int npoints, Point2f *points0, Point2f *points1, float *rmse)
{
    Mat_<float> T0 = normalizePoints(npoints, points0);
    Mat_<float> T1 = normalizePoints(npoints, points1);

    Mat_<float> A(2*npoints, 6), b(2*npoints, 1);
    float *a0, *a1;
    Point2f p0, p1;

    for (int i = 0; i < npoints; ++i)
    {
        a0 = A[2*i];
        a1 = A[2*i+1];
        p0 = points0[i];
        p1 = points1[i];
        a0[0] = p0.x; a0[1] = p0.y; a0[2] = 1; a0[3] = a0[4] = a0[5] = 0;
        a1[0] = a1[1] = a1[2] = 0; a1[3] = p0.x; a1[4] = p0.y; a1[5] = 1;
        b(2*i,0) = p1.x;
        b(2*i+1,0) = p1.y;
    }

    Mat_<float> sol;
    solve(A, b, sol, DECOMP_NORMAL | DECOMP_LU);

    if (rmse)
        *rmse = static_cast<float>(norm(A*sol, b, NORM_L2) / sqrt(static_cast<double>(npoints)));

    Mat_<float> M = Mat::eye(3, 3, CV_32F);
    for (int i = 0, k = 0; i < 2; ++i)
        for (int j = 0; j < 3; ++j, ++k)
            M(i,j) = sol(k,0);

    return T1.inv() * M * T0;
}


Mat estimateGlobalMotionLeastSquares(
        InputOutputArray points0, InputOutputArray points1, int model, float *rmse)
{
    CV_Assert(model <= MM_AFFINE);
    CV_Assert(points0.type() == points1.type());
    const int npoints = points0.getMat().checkVector(2);
    CV_Assert(points1.getMat().checkVector(2) == npoints);

    typedef Mat (*Impl)(int, Point2f*, Point2f*, float*);
    static Impl impls[] = { estimateGlobMotionLeastSquaresTranslation,
                            estimateGlobMotionLeastSquaresTranslationAndScale,
                            estimateGlobMotionLeastSquaresRotation,
                            estimateGlobMotionLeastSquaresRigid,
                            estimateGlobMotionLeastSquaresSimilarity,
                            estimateGlobMotionLeastSquaresAffine };

    Point2f *points0_ = points0.getMat().ptr<Point2f>();
    Point2f *points1_ = points1.getMat().ptr<Point2f>();

    return impls[model](npoints, points0_, points1_, rmse);
}


Mat estimateGlobalMotionRansac(
        InputArray points0, InputArray points1, int model, const RansacParams &params,
        float *rmse, int *ninliers)
{
    CV_Assert(model <= MM_AFFINE);
    CV_Assert(points0.type() == points1.type());
    const int npoints = points0.getMat().checkVector(2);
    CV_Assert(points1.getMat().checkVector(2) == npoints);

    const Point2f *points0_ = points0.getMat().ptr<Point2f>();
    const Point2f *points1_ = points1.getMat().ptr<Point2f>();
    const int niters = params.niters();

    // current hypothesis
    vector<int> indices(params.size);
    vector<Point2f> subset0(params.size);
    vector<Point2f> subset1(params.size);

    // best hypothesis
    vector<Point2f> subset0best(params.size);
    vector<Point2f> subset1best(params.size);
    Mat_<float> bestM;
    int ninliersMax = -1;

    RNG rng(0);
    Point2f p0, p1;
    float x, y;

    for (int iter = 0; iter < niters; ++iter)
    {
        for (int i = 0; i < params.size; ++i)
        {
            bool ok = false;
            while (!ok)
            {
                ok = true;
                indices[i] = static_cast<unsigned>(rng) % npoints;
                for (int j = 0; j < i; ++j)
                    if (indices[i] == indices[j])
                        { ok = false; break; }
            }
        }
        for (int i = 0; i < params.size; ++i)
        {
            subset0[i] = points0_[indices[i]];
            subset1[i] = points1_[indices[i]];
        }

        Mat_<float> M = estimateGlobalMotionLeastSquares(subset0, subset1, model, 0);

        int numinliers = 0;
        for (int i = 0; i < npoints; ++i)
        {
            p0 = points0_[i];
            p1 = points1_[i];
            x = M(0,0)*p0.x + M(0,1)*p0.y + M(0,2);
            y = M(1,0)*p0.x + M(1,1)*p0.y + M(1,2);
            if (sqr(x - p1.x) + sqr(y - p1.y) < params.thresh * params.thresh)
                numinliers++;
        }
        if (numinliers >= ninliersMax)
        {
            bestM = M;
            ninliersMax = numinliers;
            subset0best.swap(subset0);
            subset1best.swap(subset1);
        }
    }

    if (ninliersMax < params.size)
        // compute RMSE
        bestM = estimateGlobalMotionLeastSquares(subset0best, subset1best, model, rmse);
    else
    {
        subset0.resize(ninliersMax);
        subset1.resize(ninliersMax);
        for (int i = 0, j = 0; i < npoints; ++i)
        {
            p0 = points0_[i];
            p1 = points1_[i];
            x = bestM(0,0)*p0.x + bestM(0,1)*p0.y + bestM(0,2);
            y = bestM(1,0)*p0.x + bestM(1,1)*p0.y + bestM(1,2);
            if (sqr(x - p1.x) + sqr(y - p1.y) < params.thresh * params.thresh)
            {
                subset0[j] = p0;
                subset1[j] = p1;
                j++;
            }
        }
        bestM = estimateGlobalMotionLeastSquares(subset0, subset1, model, rmse);
    }

    if (ninliers)
        *ninliers = ninliersMax;

    return bestM;
}


MotionEstimatorRansacL2::MotionEstimatorRansacL2(MotionModel model)
    : MotionEstimatorBase(model)
{
    setRansacParams(RansacParams::default2dMotion(model));
    setMinInlierRatio(0.1f);
}


Mat MotionEstimatorRansacL2::estimate(InputArray points0, InputArray points1, bool *ok)
{
#ifdef TimeDebug
	double t = getTickCount();
#endif // TimeDebug

    CV_Assert(points0.type() == points1.type());
    const int npoints = points0.getMat().checkVector(2);
    CV_Assert(points1.getMat().checkVector(2) == npoints);
	Mat_<float> M;
	int ninliers = 0;
    // find motion
#ifdef MYHOMOGRAPHY
	Mat src=points0.getMat();
	Mat dst=points1.getMat();
	ocl::oclMat test(src);
	ocl::Context *clCxt= test.clCxt;
	cl_context context = *((cl_context*)clCxt->getOpenCLContextPtr());
#endif // MM_HOMOGRAPHY

    if (motionModel() != MM_HOMOGRAPHY)
            M = estimateGlobalMotionRansac(
                    points0, points1, motionModel(), ransacParams_, 0, &ninliers);
    else
    {
        vector<uchar> mask;

#ifdef MYHOMOGRAPHY
		M = ocl::findHomography(src,dst,3.0,512,&ninliers);
#else
        M = findHomography(points0, points1, mask, CV_RANSAC);
		

    for (int i  = 0; i < npoints; ++i)
            if (mask[i]) ninliers++;
#endif // MYHOMOGRAPHY 
	}

    // check if we're confident enough in estimated motion

    if (ok) *ok = true;

	//minInlierRatio_ = 0.8;
    if (static_cast<float>(ninliers) / npoints < minInlierRatio_)
    {
        M = Mat::eye(3, 3, CV_32F);
        if (ok) *ok = false;
#ifdef TimeDebug
		cout << "estimate bad:"<<((double)getTickCount()-t)/getTickFrequency()<<endl;
#endif // TimeDebug
		reflash_flag = true;

    }
#ifdef TimeDebug
	cout << "estimate:"<<((double)getTickCount()-t)/getTickFrequency()<<endl;
#endif // TimeDebug

    return M;
}


MotionEstimatorL1::MotionEstimatorL1(MotionModel model)
    : MotionEstimatorBase(model)
{
}


// TODO will estimation of all motions as one LP problem be faster?
Mat MotionEstimatorL1::estimate(InputArray points0, InputArray points1, bool *ok)
{
    CV_Assert(points0.type() == points1.type());
    const int npoints = points0.getMat().checkVector(2);
    CV_Assert(points1.getMat().checkVector(2) == npoints);

#ifndef HAVE_CLP

    CV_Error(CV_StsError, "The library is built without Clp support");
    if (ok) *ok = false;
    return Mat::eye(3, 3, CV_32F);

#else

    CV_Assert(motionModel() <= MM_AFFINE && motionModel() != MM_RIGID);

    // prepare LP problem

    const Point2f *points0_ = points0.getMat().ptr<Point2f>();
    const Point2f *points1_ = points1.getMat().ptr<Point2f>();

    int ncols = 6 + 2*npoints;
    int nrows = 4*npoints;

    if (motionModel() == MM_SIMILARITY)
        nrows += 2;
    else if (motionModel() == MM_TRANSLATION_AND_SCALE)
        nrows += 3;
    else if (motionModel() == MM_TRANSLATION)
        nrows += 4;

    rows_.clear();
    cols_.clear();
    elems_.clear();
    obj_.assign(ncols, 0);
    collb_.assign(ncols, -INF);
    colub_.assign(ncols, INF);

    int c = 6;

    for (int i = 0; i < npoints; ++i, c += 2)
    {
        obj_[c] = 1;
        collb_[c] = 0;

        obj_[c+1] = 1;
        collb_[c+1] = 0;
    }

    elems_.clear();
    rowlb_.assign(nrows, -INF);
    rowub_.assign(nrows, INF);

    int r = 0;
    Point2f p0, p1;

    for (int i = 0; i < npoints; ++i, r += 4)
    {
        p0 = points0_[i];
        p1 = points1_[i];

        set(r, 0, p0.x); set(r, 1, p0.y); set(r, 2, 1); set(r, 6+2*i, -1);
        rowub_[r] = p1.x;

        set(r+1, 3, p0.x); set(r+1, 4, p0.y); set(r+1, 5, 1); set(r+1, 6+2*i+1, -1);
        rowub_[r+1] = p1.y;

        set(r+2, 0, p0.x); set(r+2, 1, p0.y); set(r+2, 2, 1); set(r+2, 6+2*i, 1);
        rowlb_[r+2] = p1.x;

        set(r+3, 3, p0.x); set(r+3, 4, p0.y); set(r+3, 5, 1); set(r+3, 6+2*i+1, 1);
        rowlb_[r+3] = p1.y;
    }

    if (motionModel() == MM_SIMILARITY)
    {
        set(r, 0, 1); set(r, 4, -1); rowlb_[r] = rowub_[r] = 0;
        set(r+1, 1, 1); set(r+1, 3, 1); rowlb_[r+1] = rowub_[r+1] = 0;
    }
    else if (motionModel() == MM_TRANSLATION_AND_SCALE)
    {
        set(r, 0, 1); set(r, 4, -1); rowlb_[r] = rowub_[r] = 0;
        set(r+1, 1, 1); rowlb_[r+1] = rowub_[r+1] = 0;
        set(r+2, 3, 1); rowlb_[r+2] = rowub_[r+2] = 0;
    }
    else if (motionModel() == MM_TRANSLATION)
    {
        set(r, 0, 1); rowlb_[r] = rowub_[r] = 1;
        set(r+1, 1, 1); rowlb_[r+1] = rowub_[r+1] = 0;
        set(r+2, 3, 1); rowlb_[r+2] = rowub_[r+2] = 0;
        set(r+3, 4, 1); rowlb_[r+3] = rowub_[r+3] = 1;
    }

    // solve

    CoinPackedMatrix A(true, &rows_[0], &cols_[0], &elems_[0], elems_.size());
    A.setDimensions(nrows, ncols);

    ClpSimplex model(false);
    model.loadProblem(A, &collb_[0], &colub_[0], &obj_[0], &rowlb_[0], &rowub_[0]);

    ClpDualRowSteepest dualSteep(1);
    model.setDualRowPivotAlgorithm(dualSteep);
    model.scaling(1);

    model.dual();

    // extract motion

    const double *sol = model.getColSolution();

    Mat_<float> M = Mat::eye(3, 3, CV_32F);
    M(0,0) = sol[0];
    M(0,1) = sol[1];
    M(0,2) = sol[2];
    M(1,0) = sol[3];
    M(1,1) = sol[4];
    M(1,2) = sol[5];

    if (ok) *ok = true;
    return M;
#endif
}


FromFileMotionReader::FromFileMotionReader(const string &path)
    : ImageMotionEstimatorBase(MM_UNKNOWN)
{
    file_.open(path.c_str());
    CV_Assert(file_.is_open());
}


Mat FromFileMotionReader::estimate(const Mat &/*frame0*/, const Mat &/*frame1*/, bool *ok)
{
    Mat_<float> M(3, 3);
    bool ok_;
    file_ >> M(0,0) >> M(0,1) >> M(0,2)
          >> M(1,0) >> M(1,1) >> M(1,2)
          >> M(2,0) >> M(2,1) >> M(2,2) >> ok_;
    if (ok) *ok = ok_;
    return M;
}


ToFileMotionWriter::ToFileMotionWriter(const string &path, Ptr<ImageMotionEstimatorBase> estimator)
    : ImageMotionEstimatorBase(estimator->motionModel()), motionEstimator_(estimator)
{
    file_.open(path.c_str());
    CV_Assert(file_.is_open());
}


Mat ToFileMotionWriter::estimate(const Mat &frame0, const Mat &frame1, bool *ok)
{
    bool ok_;
    Mat_<float> M = motionEstimator_->estimate(frame0, frame1, &ok_);
    file_ << M(0,0) << " " << M(0,1) << " " << M(0,2) << " "
          << M(1,0) << " " << M(1,1) << " " << M(1,2) << " "
          << M(2,0) << " " << M(2,1) << " " << M(2,2) << " " << ok_ << endl;
    if (ok) *ok = ok_;
    return M;
}


KeypointBasedMotionEstimator::KeypointBasedMotionEstimator(Ptr<MotionEstimatorBase> estimator)
    : ImageMotionEstimatorBase(estimator->motionModel()), motionEstimator_(estimator)
{
    setDetector(new GoodFeaturesToTrackDetector());
    setOpticalFlowEstimator(new SparsePyrLkOptFlowEstimator());
    setOutlierRejector(new NullOutlierRejector());
}



Mat KeypointBasedMotionEstimator::estimate(const Mat &frame0, const Mat &frame1, bool *ok)
{
    // find keypoints
#ifdef TimeDebug
	double t= getTickCount();
#endif // TimeDebug
    detector_->detect(frame0, keypointsPrev_);

#ifdef TimeDebug
	cout << "detector:"<<((double)getTickCount()-t)/getTickFrequency()<<endl;
#endif
    // extract points from keypoints
    pointsPrev_.resize(keypointsPrev_.size());
    for (size_t i = 0; i < keypointsPrev_.size(); ++i)
    	pointsPrev_[i] = keypointsPrev_[i].pt;

    // find correspondences
#ifdef TimeDebug
	 t= getTickCount();
#endif // TimeDebug

    optFlowEstimator_->run(frame0, frame1, pointsPrev_, points_, status_, noArray());
#ifdef TimeDebug
	cout << "optflow:"<<((double)getTickCount()-t)/getTickFrequency()<<endl;
#endif
    // leave good correspondences only

    pointsPrevGood_.clear(); pointsPrevGood_.reserve(points_.size());
    pointsGood_.clear(); pointsGood_.reserve(points_.size());

    for (size_t i = 0; i < points_.size(); ++i)
    {
        if (status_[i])
        {
            pointsPrevGood_.push_back(pointsPrev_[i]);
            pointsGood_.push_back(points_[i]);
        }
    }

    // perform outlier rejection

    IOutlierRejector *outlRejector = static_cast<IOutlierRejector*>(outlierRejector_);
    if (!dynamic_cast<NullOutlierRejector*>(outlRejector))
    {
        pointsPrev_.swap(pointsPrevGood_);
        points_.swap(pointsGood_);

        outlierRejector_->process(frame0.size(), pointsPrev_, points_, status_);

        pointsPrevGood_.clear();
        pointsPrevGood_.reserve(points_.size());

        pointsGood_.clear();
        pointsGood_.reserve(points_.size());

        for (size_t i = 0; i < points_.size(); ++i)
        {
            if (status_[i])
            {
                pointsPrevGood_.push_back(pointsPrev_[i]);
                pointsGood_.push_back(points_[i]);
            }
        }
    }

    // estimate motion
    return motionEstimator_->estimate(pointsPrevGood_, pointsGood_, ok);
}


#ifdef HAVE_OPENCV_GPU
KeypointBasedMotionEstimatorGpu::KeypointBasedMotionEstimatorGpu(Ptr<MotionEstimatorBase> estimator)
    : ImageMotionEstimatorBase(estimator->motionModel()), motionEstimator_(estimator)
{
    CV_Assert(gpu::getCudaEnabledDeviceCount() > 0);
    setOutlierRejector(new NullOutlierRejector());
}


Mat KeypointBasedMotionEstimatorGpu::estimate(const Mat &frame0, const Mat &frame1, bool *ok)
{
    frame0_.upload(frame0);
    frame1_.upload(frame1);
    return estimate(frame0_, frame1_, ok);
}



Mat KeypointBasedMotionEstimatorGpu::estimate(const gpu::GpuMat &frame0, const gpu::GpuMat &frame1, bool *ok)
{
    // convert frame to gray if it's color

    gpu::GpuMat grayFrame0;
    if (frame0.channels() == 1)
        grayFrame0 = frame0;
    else
    {
        gpu::cvtColor(frame0, grayFrame0_, CV_BGR2GRAY);
        grayFrame0 = grayFrame0_;
    }

#ifdef TimeDebug
	double t = getTickCount();
#endif // TimeDebug
//find keypoints
	detector_.maxCorners = 100;
	detector_(grayFrame0, pointsPrev_);
#ifdef TimeDebug
	cout << "detector:"<<((double)getTickCount()-t)/getTickFrequency()<<" ";
#endif
	
#ifdef TimeDebug
	t = getTickCount();
#endif
	// find correspondences
	optFlowEstimator_.run(frame0, frame1, pointsPrev_, points_, status_);
#ifdef TimeDebug
	cout << "optFlow:"<<((double)getTickCount()-t)/getTickFrequency()<<endl;
#endif

#ifdef TimeDebug
	t = getTickCount();
#endif // TimeDebug	

    // leave good correspondences only
    gpu::compactPoints(pointsPrev_, points_, status_);

#ifdef TimeDebug
	cout << "compact:"<<((double)getTickCount()-t)/getTickFrequency()<<endl;
#endif

    pointsPrev_.download(hostPointsPrev_);
    points_.download(hostPoints_);

	

    // perform outlier rejection

    IOutlierRejector *rejector = static_cast<IOutlierRejector*>(outlierRejector_);
    if (!dynamic_cast<NullOutlierRejector*>(rejector))
    {
        outlierRejector_->process(frame0.size(), hostPointsPrev_, hostPoints_, rejectionStatus_);

        hostPointsPrevTmp_.clear();
        hostPointsPrevTmp_.reserve(hostPoints_.cols);

        hostPointsTmp_.clear();
        hostPointsTmp_.reserve(hostPoints_.cols);

        for (int i = 0; i < hostPoints_.cols; ++i)
        {
            if (rejectionStatus_[i])
            {
                hostPointsPrevTmp_.push_back(hostPointsPrev_.at<Point2f>(0,i));
                hostPointsTmp_.push_back(hostPoints_.at<Point2f>(0,i));
            }
        }

        hostPointsPrev_ = Mat(1, (int)hostPointsPrevTmp_.size(), CV_32FC2, &hostPointsPrevTmp_[0]);
        hostPoints_ = Mat(1, (int)hostPointsTmp_.size(), CV_32FC2, &hostPointsTmp_[0]);
    }

    // estimate motion
    return motionEstimator_->estimate(hostPointsPrev_, hostPoints_, ok);
}
#endif // HAVE_OPENCV_GPU


#ifdef HAVE_OPENCL

KeypointBasedMotionEstimatorOcl::KeypointBasedMotionEstimatorOcl(Ptr<MotionEstimatorBase> estimator)
	: ImageMotionEstimatorBase(estimator->motionModel()), motionEstimator_(estimator)
{/*
#ifdef Intel
	CV_Assert(cv::ocl::Context::getContext()->getDeviceInfo().isIntelDevice);
#else
	CV_Assert(!cv::ocl::Context::getContext()->getDeviceInfo().isIntelDevice);*/
//#endif // Intel
	setOutlierRejector(new NullOutlierRejector());
}



Mat KeypointBasedMotionEstimatorOcl::estimate(const ocl::oclMat &frame0, const ocl::oclMat &frame1, bool *ok)
{
	// convert frame to gray if it's color

	ocl::oclMat grayFrame0;
	if (frame0.channels() == 1)
		grayFrame0 = frame0;
	else
	{
		ocl::cvtColor(frame0, grayFrame0_, CV_BGR2GRAY);
		grayFrame0 = grayFrame0_;
	}

	// find keypoints
#ifdef TimeDebug
	double t = getTickCount();
#endif // TimeDebug
	detector_(grayFrame0, pointsPrev_);
#ifdef TimeDebug
	cout << "detector:"<<((double)getTickCount()-t)/getTickFrequency()<<" ";
#endif
	// find correspondences
#ifdef TimeDebug
	t = getTickCount();
#endif
	optFlowEstimator_.run(frame0, frame1, pointsPrev_, points_, status_);
#ifdef TimeDebug
	cout << "optFlow:"<<((double)getTickCount()-t)/getTickFrequency()<<endl;
#endif
	// leave good correspondences only

//	ocl::compactPoints(pointsPrev_, points_, status_);

	pointsPrev_.download(hostPointsPrev_);
	points_.download(hostPoints_);

	// perform outlier rejection

	IOutlierRejector *rejector = static_cast<IOutlierRejector*>(outlierRejector_);
#ifdef MYHOMOGRAPHY
	hostPointsPrevTmp_.clear();
	hostPointsPrevTmp_.reserve(hostPoints_.cols);

	hostPointsTmp_.clear();
	hostPointsTmp_.reserve(hostPoints_.cols);

	for (int i = 0; i < hostPoints_.cols; ++i)
	{
			hostPointsPrevTmp_.push_back(hostPointsPrev_.at<Point2f>(0,i));
			hostPointsTmp_.push_back(hostPoints_.at<Point2f>(0,i));
	}

	hostPointsPrev_ = Mat(1, (int)hostPointsPrevTmp_.size(), CV_32FC2, &hostPointsPrevTmp_[0]);
	hostPoints_ = Mat(1, (int)hostPointsTmp_.size(), CV_32FC2, &hostPointsTmp_[0]);
#endif // MYHOMOGRAPHY

	if (!dynamic_cast<NullOutlierRejector*>(rejector))
	{
		outlierRejector_->process(frame0.size(), hostPointsPrev_, hostPoints_, rejectionStatus_);

		hostPointsPrevTmp_.clear();
		hostPointsPrevTmp_.reserve(hostPoints_.cols);

		hostPointsTmp_.clear();
		hostPointsTmp_.reserve(hostPoints_.cols);

		for (int i = 0; i < hostPoints_.cols; ++i)
		{
			if (rejectionStatus_[i])
			{
				hostPointsPrevTmp_.push_back(hostPointsPrev_.at<Point2f>(0,i));
				hostPointsTmp_.push_back(hostPoints_.at<Point2f>(0,i));
			}
		}

		hostPointsPrev_ = Mat(1, (int)hostPointsPrevTmp_.size(), CV_32FC2, &hostPointsPrevTmp_[0]);
		hostPoints_ = Mat(1, (int)hostPointsTmp_.size(), CV_32FC2, &hostPointsTmp_[0]);
	}

//

	// estimate motion
	return motionEstimator_->estimate(hostPointsPrev_, hostPoints_, ok);
}

Mat KeypointBasedMotionEstimatorOcl::estimate(const Mat &frame0,const Mat &frame1,bool *ok)
{
	frame0_.upload(frame0);
	frame1_.upload(frame1);
	return estimate(frame0_,frame1_,ok);

}
#endif // HAVE_OPENCL



Mat getMotion(int from, int to, const vector<Mat> &motions)
{
    Mat M = Mat::eye(3, 3, CV_32F);
    if (to > from)
    {
        for (int i = from; i < to; ++i)
            M = at(i, motions) * M;
    }
    else if (from > to)
    {
        for (int i = to; i < from; ++i)
            M = at(i, motions) * M;
        M = M.inv();
    }
    return M;
}



} // namespace videostab
} // namespace cv


