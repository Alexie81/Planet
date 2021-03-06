#include "SplineEvaluation.hpp"
#include "boost-hdrs/remez.hpp"
#include <boost/math/tools/polynomial.hpp>
#include <iostream>
#include <fstream>
#include <algorithm>
#include <cstring>
#include <cmath>
#include <vector>
#include <iomanip>
#include <cstdlib>
#include <map>
#include <chrono>
#include <iomanip>
#include <cassert>
#include <unordered_map>
#include <cassert>
#include <random>
#include <algorithm>
#include <immintrin.h>
//---------------------------------------------------------------------------
using namespace std;
//---------------------------------------------------------------------------
typedef std::pair<double,double> Coord;
typedef std::pair<double, unsigned> segCoord;
typedef btree::btree_map<double, double> btree_map;
typedef btree::btree_map<double, unsigned> btree_map_segments;
//---------------------------------------------------------------------------
// modes for splines
#define WORK_ON_SPLINE_MODE 1
#define SSE_SPLIT_MODE 0
#define JUST_FIT_MODE 1
#define LOWER_BOUND_MODE 2
#define MAP_MODE 3
#define BTREE_MODE 4
#define BTREE_SPLINE_MODE 5
#define TEST_MODE_ACTIVATED 1
//---------------------------------------------------------------------------
const double precision = std::numeric_limits<double>::epsilon();
const double MIN_ANGLE = 1e-16; // 10 ^ (-precisionOfDouble)
const double MAX_RUNS = 2;
//---------------------------------------------------------------------------
double globalDiff;
double powers[2]; // for sse or avx
std::vector<segCoord> globalSpline; // we need it to define the function for the Remez algorithm
static double interpolate(const vector<Coord>& spline, double pos);
static std::vector<Coord> preciseApproximation(const std::vector<Coord>& function, unsigned desiredSize);
static pair<double, double> computeOverallPrecision(const std::vector<Coord>& func, const std::vector<Coord>& reduced);
//---------------------------------------------------------------------------
SplineEvaluation::SplineEvaluation() {
}
//---------------------------------------------------------------------------
SplineEvaluation::SplineEvaluation(const std::vector<Coord>& function, unsigned desiredSize, const unsigned useSplineMode, const unsigned searchMode, const unsigned testMode) {
    if (useSplineMode == WORK_ON_SPLINE_MODE) {
        // Compute the spline
        this->spline = preciseApproximation(function, min(desiredSize, this->limitSplineSize));
        // Get the errors
        pair<double, double> errors = computeOverallPrecision(function, this->spline);
        cerr << "maxError = " << errors.first << " avgError = " << errors.second << endl;
#if 0
        cerr << "Print spline : " << endl;
        for (auto coord: this->spline)
            cerr << "(" << coord.first << "," << coord.second << ")" << endl;
#endif
        switch (searchMode) {
            case SSE_SPLIT_MODE : {
                cerr << "enter Split Mode" << endl;
                // separate the spline into countOfBuckets smaller splines
                splitSegmentSpline();
                this->polyBuckets = new long double*[countOfBuckets];
                
                // fit each spline
                for (unsigned index = 0; index < this->countOfBuckets; ++index) {
                    globalSpline = this->splineBuckets[index];
                    changeIntoArray(&this->polyBuckets[index], fitSpline(this->splineBuckets[index]));
                }
                cerr << "quit Split Mode" << endl;
                break;
            }
            case JUST_FIT_MODE : {
                cerr << "enter just fit mode" << endl;
                // get the segment spline
                transformSpline();

                // now fit it
                globalSpline = this->segmentSpline;
                changeIntoArray(&this->poly, fitSpline(this->segmentSpline));
                cerr << "quit just fit mode" << endl;
                break;
            }
            case BTREE_SPLINE_MODE : {
                cerr << "enter btree spline mode" << endl;
                transformSpline();
                this->btreeSegments = new btree_map_segments; 
                for (auto coord: this->segmentSpline) {
                    this->btreeSegments->insert(coord);
                }
                cerr << "done with btree spline mode" << endl;
                break;
            }
        }
        // Pad the spline with dummy values
        convertValues();
    } else {
        switch (searchMode) {
            case BTREE_MODE : {
                cerr << "Enter values in btree" << endl;
                this->btreeMap = new btree_map;
                for (auto coord: function) {
                    this->btreeMap->insert(coord);
                }
                cerr << "Done with btree" << endl;
                if (testMode != TEST_MODE_ACTIVATED)
                    break;
            }
            case MAP_MODE : {
                cerr << "Enter the values into map" << endl;
                for (auto coord: function) {
                    this->coordinates[coord.first] = coord.second;
                }
                cerr << "Done with the map" << endl;
                break;
            }
        }
    }
}
//---------------------------------------------------------------------------
SplineEvaluation::~SplineEvaluation() 
// deconstructor
{
    // TODO: save the modes, in order to know what to free
    // there are only 4 pointers, which have to be deleted (spl)
    // There may be a problem with "new" on vector.
}
//---------------------------------------------------------------------------
void SplineEvaluation::splitSegmentSpline() 
// split the segment spline into 5 smaller splines
{
    // alloc the smaller splines
    this->splineBuckets = new std::vector<segCoord>[countOfBuckets];
    
    // Get the index of the first split
    unsigned splittedPos = this->spline.size() / countOfBuckets;
    unsigned ptr = splittedPos, segmentCount = 0, splineSize = this->spline.size();
    for (unsigned splineOrder = 0; splineOrder < countOfBuckets; ++splineOrder) {
        // Construct the next smaller spline
        for (unsigned index = ptr - splittedPos; (index < ptr) && (index < splineSize); ++index) {
            this->splineBuckets[splineOrder].push_back(make_pair(this->spline[index].first, segmentCount++));
        }
        // add the next splitted position
        if (splineOrder != this->countOfBuckets - 1)
            this->splittedPositions[splineOrder] = this->spline[segmentCount - 1].first;
        // go to the next spline
        ptr += splittedPos;
    }
    // Load the ymm-register with the limits of the splines
    saveLimits = _mm_loadu_pd(this->splittedPositions);
}
//---------------------------------------------------------------------------
SplineEvaluation::SplineEvaluation(unsigned size, const double* x, const double* y) {
    this->spline.resize(size);
    for (unsigned index = 0; index < size; ++index)
        this->spline[index] = make_pair(x[index], y[index]);
}
//---------------------------------------------------------------------------
void SplineEvaluation::save(std::ofstream& file_out) 
// save in binary file the spline and the polynomial
{
    file_out.write(reinterpret_cast<char*>(this->spline.data()),this->spline.size()*sizeof(pair<double,double>));
    file_out.write(reinterpret_cast<char*>(this->poly),(polyGrade + 1) * sizeof(long double));
}
//---------------------------------------------------------------------------
void SplineEvaluation::load(std::ifstream& file_in) 
// load from binary file the spline and the polynomial
{
    auto pos = file_in.tellg();
    file_in.seekg( 0, ios::end );  
    auto size=file_in.tellg()-pos;
    file_in.seekg(0,ios::beg);

    unsigned splineSize = ((size / sizeof(double)) - (polyGrade + 1)) / 2;
    
    this->spline.resize(splineSize);
    this->poly = new long double[polyGrade + 1];
    
    file_in.read(reinterpret_cast<char*>(this->spline.data()),splineSize * sizeof(pair<double,double>));
    file_in.read(reinterpret_cast<char*>(this->poly),(polyGrade + 1)*sizeof(long double));
    convertValues();
}
//---------------------------------------------------------------------------
void SplineEvaluation::transformSpline() 
// marks every x with the segment where it is to be found. This is called from now on a "segment Spline"
{
    unsigned segmentCount = 0;
    for (auto elem: this->spline)
        this->segmentSpline.push_back(make_pair(elem.first, segmentCount++));
}
//---------------------------------------------------------------------------
unsigned SplineEvaluation::size() const {
    return this->spline.size();
}
//---------------------------------------------------------------------------
static double square(double x) {
    return x * x;
}
//---------------------------------------------------------------------------
static bool greaterThan(double x, double y) {
    return (x - y) > precision;
}
//---------------------------------------------------------------------------
static bool equals(double x, double y) {
    return fabs(x - y) < precision;
}
//---------------------------------------------------------------------------
static double distance(Coord a, Coord b) 
// returns euclidian distance between both points
{
    return sqrt(square(a.first - b.first) + square(a.second - b.second));
}
//---------------------------------------------------------------------------
static double cosinus(Coord u, Coord v, Coord w) 
// computes cos(angle) between (u, v, w) with v in middle
{
    double a = distance(u, w), b = distance(u, v), c = distance(v, w);
    return (square(a) - square(b) - square(c)) / (2.0 * b * c);
}
//---------------------------------------------------------------------------
static double computeAngle(Coord u, Coord v, Coord w) 
// computes angle between (u, v, w) using cosinus
{
    return 1.0 - cosinus(u, v, w);
}
//---------------------------------------------------------------------------
static double determinant(Coord u, Coord v, Coord w) 
// computes the determinant, which represents the spanned area.
{
    return (v.first - u.first) * (w.second - u.second) - (v.second - u.second) * (w.first - u.first);
}
//---------------------------------------------------------------------------
static int orientation(Coord u, Coord v, Coord w) 
// computes in which direction w is placed in respect to (u, v)
{
    double det = determinant(u, v, w);
    if (det < -precision)
        return -1;
    else if (det > precision)
        return 1;
    else
        return 0;
}
//---------------------------------------------------------------------------
static pair<double, double> computeOverallPrecision(const std::vector<Coord>& func, const std::vector<Coord>& reduced)
// returns (maxError, avgError)
{
    // compute errors
    double sumError=0, maxError=0, relError = 0;
    for (auto e: func) {
        double estimate=interpolate(reduced, e.first);
        double real=e.second;
        double diff=estimate-real;
        if (diff<0) 
            diff=-diff;
        if (diff>maxError)
            maxError=diff;
        sumError+=diff;
        // relError += (!real) ? diff : diff / real;
    }
    // relError /= func.size();
    sumError /= func.size();
    return make_pair(maxError, sumError);
}
#if 0
//---------------------------------------------------------------------------
static double inflexionAngleOfFunction(const std::vector<Coord>& data) 
// get an angle which creates small inflexions 
{
    if (data.size() == 0) {
        cerr << "invalid data!" << endl;
        assert(0);
    }
    if (data.size() == 1)
        return 0;
   // Taut the string
   std::vector<Coord>::const_iterator iter = data.begin(), limit = data.end();
   Coord prev = *iter, curr = *(++iter);
   double inflexionAngle = 0, currSum = 0;
   
    for (++iter; iter != limit; ++iter) {
        double deviation = 0.0;
        int orient = orientation(prev, curr, *iter);
        if (orient)
            deviation = computeAngle(prev, curr, *iter);
        currSum += orient * deviation;
        
        // update the indexes.
        prev = curr;
        curr = *iter;
    }
    return fabs(currSum) / data.size();
}
#endif
//---------------------------------------------------------------------------
static std::vector<Coord> angleApproximation(const std::vector<Coord>& data, double maxAngle, double maxDistance = 0)
// Compress the functions by storing only the inflections with angle lower than maxAngle
{
   // Add the first point
   std::vector<Coord> result;
   if (data.empty())
      return result;
   result.push_back(data[0]);
   if (data.size()==1)
      return result;

   // Taut the string
   std::vector<Coord>::const_iterator iter = data.begin(), limit = data.end();
   Coord prev = *iter, curr = *(++iter);
   double currSum = 0;
   
    for (++iter; iter != limit; ++iter) {
        double deviation = 0.0;
        int orient = orientation(prev, curr, *iter);
        if (orient) {
            deviation = computeAngle(prev, curr, *iter);
            currSum += orient * deviation;
        }

        // Check for inflexion.
        if (greaterThan(deviation, maxAngle) || greaterThan(fabs(currSum), maxAngle)) { 
            result.push_back(curr);
            currSum = 0;
        }
        
        // update the indexes.
        prev = curr;
        curr = *iter;
    }
    // Add the last point.
    if (result.back() != *(--data.end()))
        result.push_back(*(--data.end()));
    return result;
}
//---------------------------------------------------------------------------
static int cmpDevs(const Coord& a,const Coord& b,const Coord& c)
   // Compare derivations. -1 if (a->b) is more swallow than (a->c), +1 if it is more steeper
{
   double dx1=b.first-a.first,dx2=c.first-a.first;
   double dy1=b.second-a.second,dy2=c.second-a.second;
   if ((dy1*dx2)<(dy2*dx1)) return -1;
   if ((dy1*dx2)>(dy2*dx1)) return 1;
   return 0;
}
//---------------------------------------------------------------------------
static std::vector<Coord> tautString(const std::vector<Coord>& data, double epsilon)
{
   // Add the first point
   std::vector<Coord> result;
   if (data.empty())
      return result;
   result.push_back(data[0]);
   if (data.size()==1)
      return result;

   // Taut the string
   std::vector<Coord>::const_iterator iter=data.begin(),limit=data.end();
   Coord upperLimit,lowerLimit,last=result.back();

   for (++iter;iter!=limit;++iter) {
      // Add the new bounds
      Coord u = *iter, l = *iter, b=result.back();
      u.second+=epsilon; l.second-=epsilon;

      // Check if we cut the error corridor
      if ((last!=b)&&((cmpDevs(b,upperLimit,*iter)<0)||(cmpDevs(b,lowerLimit,*iter)>0))) {
         result.push_back(last);
         b=last;
        }

      // Update the error margins
      if ((last==b)||(cmpDevs(b,upperLimit,u)>0))
         upperLimit=u;
      if ((last==b)||(cmpDevs(b,lowerLimit,l)<0))
        lowerLimit=l;

      // And remember the current point
      last=*iter;
   }

   // Add the last point
   result.push_back(*(--data.end()));
   return result;
}
//---------------------------------------------------------------------------
static std::vector<Coord> compressFunction(const std::vector<Coord>& function,unsigned desiredSize)
// Compress to the desired size
{
   // Relax a bit to speed up compression
   unsigned maxSize=desiredSize+(desiredSize/100),minSize=desiredSize-(desiredSize/100);

   // Fits?
   if (function.size()<=maxSize)
      return function;

   // No, binary search
   unsigned left=0,right=function.size();
   while (left<right) {
      unsigned middle=(left+right)/2;
      std::vector<Coord> candidate=tautString(function, middle);
      if (candidate.size()<minSize) {
         right=middle;
      } else if (candidate.size()>maxSize) {
         left=middle+1;
      } else {
        return candidate;
    }
   }
   // Final call, this is the best we could get
   return tautString(function, left);
}
//---------------------------------------------------------------------------
static std::vector<Coord> preciseApproximation(const std::vector<Coord>& function, unsigned desiredSize)
// first use a sieve for the smallest angles, then apply the previous algorithm
{
    // Apply the last algorithm to compare its results with the new ones
    std::vector<Coord> candidate = compressFunction(function, desiredSize);
    pair<double, double> previousAlg = computeOverallPrecision(function, candidate);
    double previousMaxError = previousAlg.first, previousAvgError = previousAlg.second;

    cerr << "previous has " << candidate.size() << "points and errors = " << previousMaxError << " and " << previousAvgError << endl;

    // let 0 if you want to search for a better spline (which may take 1-2 minutes)
#if 1
    return candidate;
#endif
    
#if 0
    // Map the sizes of the splines to pairs of (maxError, avgError)
    unordered_map<int, pair<int, pair<double, double>>> answers;
    answers[candidate.size()] = make_pair(candidate.size(), previousAlg);
#endif
    // it could be modified, for precision reasons
    double currAngle = MIN_ANGLE;
    bool foundBetter = true;
    int runs = MAX_RUNS;

    double smallestAvgError = previousAvgError, smallestMaxError = previousMaxError, cand = -1;
    for (int runs = MAX_RUNS; (runs--) || (foundBetter); currAngle *= 10) {
        foundBetter = false;
        
        cerr << "progress = " << runs << " with " << currAngle << endl;
        for (unsigned mul = 2; mul < 20; ++mul) {
            double eps = currAngle * mul / 2;
            
            // Sharp the function then apply the previous algorithm
            auto samplePoints = angleApproximation(function, eps);
            samplePoints = compressFunction(samplePoints, desiredSize);
            
            pair<double, double> errors = computeOverallPrecision(function, samplePoints);
            double maxError = errors.first, avgError = errors.second;
            
            cerr << "check with " << eps << " errors = " << maxError << " " << avgError << endl;
            
            if ((previousAvgError > avgError) && (previousMaxError > maxError)) {   
#if 1
                cerr << "count Points : " << samplePoints.size() << endl;
                cerr << "it's better at " << eps << " with " << maxError << " and " << avgError << endl;
                cerr << "rate for avg is " << ((-avgError + previousAvgError) / previousAvgError) * 100 << endl;
                cerr << "rate for max is " << ((-maxError + previousMaxError) / previousMaxError) * 100 << endl;
#endif
                
#if 0
                // Check the existance of this size
                if (answers.find(samplePoints.size()) == answers.end()) {
                    // not yet in map this size
                    candidate = compressFunction(function, samplePoints.size());
                    pair<double, double> curr = computeOverallPrecision(function, candidate);
                    
                    // And map it to its errors
                    answers[samplePoints.size()] = make_pair(candidate.size(), curr);
                }
                
                auto curr = answers.find(samplePoints.size())->second;
                
                cerr << "to " << samplePoints.size() << " found : " << curr.first << " with " << "(" << curr.second.first << "," << curr.second.second << ")" << endl;
#endif
                candidate = compressFunction(function, samplePoints.size());
                pair<double, double> curr = computeOverallPrecision(function, candidate);
            
                if (curr == previousAlg) {
                    cerr << "Even better than the new one!" << endl;
                    foundBetter = true;
                    
                    // get the best avgError
                    if (smallestAvgError > avgError) {
                        smallestAvgError = avgError;
                        smallestMaxError = maxError;
                        cand = eps;
                    } else if (equals(smallestAvgError, avgError)) {
                        if (smallestMaxError > maxError) {
                            smallestMaxError = maxError;
                            cand = eps;
                        }
                    }
                } else {
                    if (candidate.size() <= samplePoints.size())
                        cerr << "Has been beaten at " << samplePoints.size() << endl;
                }
            }
        }
    }
    if (cand == -1) {
        cerr << "Not found better than that!" << endl;
        return candidate;
    }
    
    // Get the best through the sieve of angles
    auto ret = angleApproximation(function, cand);
    ret = compressFunction(ret, desiredSize);
    return ret;
}
//---------------------------------------------------------------------------
static unsigned realSegment(const vector<Coord>& spline, double pos)
// DEBUG: get the real segment
{
    if (pos<=spline.front().first)
      return 0;
   if (pos>=spline.back().first)
      return spline.size()-1;
   auto iter=lower_bound(spline.begin(),spline.end(),pos,[](const Coord& a,double b) { return a.first<b; });
   
   // cerr << setprecision(12) << "for " << pos << "come with " << (iter-1)->first << " " << iter->first << endl;
   
   if (iter->first == pos)
       return iter - spline.begin();
   return (iter-1) - spline.begin();
}
//---------------------------------------------------------------------------
static double interpolateWithSegments(const vector<segCoord>& spline, double pos)
// Helper function for defineFunction. Evaluates the segment Spline at pos.
{
   if (pos<=spline.front().first)
      return spline.front().second;
   if (pos>=spline.back().first)
      return spline.back().second;
   auto iter=std::lower_bound(spline.begin(),spline.end(),pos,[](const Coord& a,double b) { return a.first<b; });
   if (iter->first==pos) 
       return iter->second;

   double dx=(iter+0)->first-(iter-1)->first;
   double dy=(iter+0)->second-(iter-1)->second;

   double ofs=pos-(iter-1)->first;
   return (iter-1)->second+ofs*(dy/dx);
}
//---------------------------------------------------------------------------
static double defineFunction(const double& x) 
// defines for the Remez algorithm how the spline works through interpolation
{
    return interpolateWithSegments(globalSpline, x);
}
//---------------------------------------------------------------------------
void SplineEvaluation::changeIntoArray(long double** moveHere, boost::math::tools::polynomial<long double> poly) 
// change the STL-vectors into simple arrays. Searching on them is faster.
{
    *moveHere = new long double[this->polyGrade + 1];
    // Copy the polynomial
    for (unsigned index = 0; index < poly.size(); ++index)
        (*moveHere)[index] = poly[index];
}
//---------------------------------------------------------------------------
void SplineEvaluation::convertValues() 
// pad values with values lower than the first one, respectively greater than the last one
// this enhances the binary searches, not having the overflow condition anymore
{
    this->length = this->spline.size();
#if 0
    // Compute offset as a power of 2 
    this->offset = 1;
    while (this->offset < this->length)
        this->offset <<= 1;
#endif
    this->offset = 0;
    // Alloc enough memory for the inital values and 2 offsets
    this->values = new double[offset + this->length + offset];
    
    // Pad with values lower than the first one
    for (unsigned index = 0; index < offset; ++index)
        this->values[index] = this->spline[0].first - 1;
    
    // Fill in with the initial values from the spline x-coordinates
    for (unsigned index = 0; index < this->spline.size(); ++index)
        this->values[offset + index] = this->spline[index].first;
    
    // Pad with values greater than the last one
    for (unsigned index = 0; index < offset; ++index)
        this->values[offset + index + this->length] = this->spline[this->length - 1].first + 1;
}
//---------------------------------------------------------------------------
boost::math::tools::polynomial<long double> SplineEvaluation::fitSpline(const std::vector<segCoord>& spline) 
// use Remez algorithm to fit a polynomial to spline 
{
    // Prepare parameters
    unsigned sizeOfNominator = this->polyGrade, sizeOfDenominator = 0;
    double a = spline.front().first, b = spline.back().first;
    bool pin = false, relError = false;
    int skew = 0, workingPrecision = boost::math::tools::digits<long double>() * 2;
    
    // Activate the algorithm with long double to get a better precision. It's recommanded to keep it so. Otherwise it could be the case
    // that an internal function from the algorithm fails because of the poor precision of double
    boost::math::tools::remez_minimax<long double> remez(defineFunction, sizeOfNominator, sizeOfDenominator, a, b, pin, relError, skew, workingPrecision);
    return remez.numerator();
}
//---------------------------------------------------------------------------
double SplineEvaluation::hornerEvaluate(double& x) const 
// evaluate the polynomial with Horner's schema
{
#if 1
    long double ret = this->poly[polyGrade];
    for (unsigned i = polyGrade; i != 0; --i)
        ret = ret * x + this->poly[i - 1];
    return static_cast<double>(ret);
#elif 1
    // Use loop unrolling which maybe is done by the compiler. Only when polyGrade = 2. Otherwise, add as many instructions as you want
    long double ret = this->poly[this->polyGrade];
    ret = ret * x + this->poly[this->polyGrade - 1];
    ret = ret * x + this->poly[this->polyGrade - 2];
    return static_cast<double>(ret);
#else
    // for regression-type of spline
    return x * this->poly[this->polyGrade] + this->poly[this->polyGrade - 1];
#endif
}
//---------------------------------------------------------------------------
double SplineEvaluation::hornerEvaluateByBuckets(unsigned bucketIndex, double& x) const 
// evaluate x in the polynomial bucketIndex
{
    long double ret = this->polyBuckets[bucketIndex][polyGrade];
    for (unsigned i = polyGrade; i != 0; --i)
        ret = ret * x + this->polyBuckets[bucketIndex][i - 1];
    return static_cast<double>(ret);
}
//---------------------------------------------------------------------------
static const int popcount[4] = {0, 1, 1, 2};
//---------------------------------------------------------------------------
double SplineEvaluation::splittedHornerEvaluate(double& x) const 
// evaluate the polynomials, depending on the spline where x lies in
{
    // load 2 x-values
    __m128d constant = _mm_set1_pd(x);
    // compare with greater than
    constant = _mm_cmpgt_pd(constant, this->saveLimits); 
    // Get the result of the comparison
    int ret = _mm_movemask_pd(constant);
    // Count how many splitter x is greater than.
    return hornerEvaluateByBuckets(popcount[ret], x);
}
//---------------------------------------------------------------------------
unsigned SplineEvaluation::binarySearch(unsigned lower, unsigned upper, double& x) const
// classic branch-free binary search over the interval [lower, upper)
{
    // dodge the branch-misses
    for (unsigned half, n = upper - lower; (n / 2) != 0; n -= half) {
        half = (n / 2);
        unsigned middle = lower + half;
        lower = ((this->values[middle] < x) ? middle : lower);
    }
    return lower;
}
//---------------------------------------------------------------------------
unsigned SplineEvaluation::searchLeft(int index, double& x) const
// search in the left part of the point "index" to exact place where x lies
{
#if 1
    // We've already padded the left part with values lower than the first element of the array
    // In this way we don't have any overflows
    int pow = 1;
    while ((index >= pow) && (this->values[index - pow] > x))
        pow *= 2;
    return SplineEvaluation::binarySearch(max(index - pow, 0), index - pow / 2, x);
#else
    // We've already padded the left part with values lower than the first element of the array
    // In this way we don't have any overflows
    // Make a fast check for the first 2 values
    if (this->values[index - 1] < x)
        return index - 1;
    if (this->values[index - 2] < x)
        return index - 2;
    
    int pow = 4;
    // Load x in an ymm register
    __m128d constant = _mm_set1_pd(x);
    leftSseExpSearch : {
        // Load the next steps
        powers[0] = this->values[index - pow];
        powers[1] = this->values[index - (pow << 1)];
        __m128d xmm = _mm_loadu_pd(powers);
        
        // Compare x with the further steps
        xmm = _mm_cmplt_pd(constant, xmm);
        // Get the result of the comparison
        int ret = _mm_movemask_pd(xmm);
        
        // If x is still lower than all steps, continue searching
        if (ret == 0x3) {
            pow <<= 2;
            goto leftSseExpSearch;
        }
        // The last used power is shifted with the count of steps which are greater than x (the number of set bits in ret)
        pow <<= ret;
    }
    // Continue with binary search, based on the last power
    // Don't forget that we padded the array, so use "offset" a the lowest index of the array
    return SplineEvaluation::binarySearch(max(index - pow, this->offset), index - pow / 2, x);
#endif    
}
//---------------------------------------------------------------------------
unsigned SplineEvaluation::searchRight(int index, double& x) const
// search in the right part of the point "index" to exact place where x lies
{
#if 1
    // We've already padded the right part with values greater than the last element of the array
    // In this way we don't have any overflows
    int pow = 1;
    while ((index + pow < this->length) && (this->values[index + pow] < x))
        pow <<= 1;
    return SplineEvaluation::binarySearch(index + pow / 2, min(index + pow, this->length - 1), x);
#else
    // We've already padded the right part with values greater than the last element of the array
    // In this way we don't have any overflows
    // Make a fast check for the first 2 values
    if (this->values[index + 1] > x)
        return index;
    if (this->values[index + 2] > x)
        return index + 1;

    int pow = 4;
    // Load x in an ymm register
    __m128d constant = _mm_set1_pd(x);
    rightSseExpSearch : {
        // Load the next steps
        powers[0] = this->values[index + pow];
        powers[1] = this->values[index + (pow << 1)];

        // Compare x with the further steps
        __m128d xmm = _mm_loadu_pd(powers);
        xmm = _mm_cmpgt_pd(constant, xmm);
        // Get the result of the comparison
        int ret = _mm_movemask_pd(xmm);
        
        // If x is still greater than all steps, continue searching
        if (ret == 0x3) {
            pow <<= 2;
            goto rightSseExpSearch;
        }
        // The last used power is shifted with the count of steps which are lower than x (the number of set bits in ret)
        pow <<= ret;
    }
    // Continue with binary search, based on the last power
    // Don't forget that we padded the array.
    return SplineEvaluation::binarySearch(index + pow / 2, min(index + pow, this->length - 1 + this->offset), x);
#endif
}
//---------------------------------------------------------------------------
static unsigned filterSegment(unsigned countOfSegments, double& segment) 
// round segment to its int value, which should lie between [0, countOfSegments - 1]
{
    if (segment < -precision) {
       return 0;
    } else if (segment > countOfSegments) {
       return countOfSegments - 1;
    }
    // round and trunc can be used interchangeably
    return static_cast<unsigned>(segment);
}
//---------------------------------------------------------------------------
unsigned SplineEvaluation::findExactSegment(double segment, double& x) const 
// find the segment where x lies
{   
    // first approximate where x could lie
    unsigned candSegment = filterSegment(this->length - 1, segment);
    
    // find the exact segment with exponential search
    if (x < this->values[candSegment]) {
        // Continue searching in the left part
#if 1
        return searchLeft(candSegment, x);
#else 
        return SplineEvaluation::binarySearch(this->offset, candSegment, x);
#endif
    } else if (x > this->values[candSegment + 1]) {
        // The condition evaluation doesn't make overflow, because noOfSegments == spline.size() - 1 
        // Continue searching in the right part
#if 1
        return searchRight(candSegment + 1, x);
#else
        return SplineEvaluation::binarySearch(candSegment + 1, this->spline.size() - 1 + this->offset, x);
#endif
    }
    // x already lies on this segment
    return candSegment;
}
//---------------------------------------------------------------------------
double SplineEvaluation::selfInterpolate(unsigned segment, double& x) const
// get f(x) at segment
{
    Coord up = this->spline[segment + 1], down = this->spline[segment];
    double dx = up.first - down.first;
    double dy = up.second - down.second;
    return down.second + (x - down.first) * (dy / dx); 
}
//---------------------------------------------------------------------------
double SplineEvaluation::chebyshevEvaluate(double& x) const 
// evaluates f(x), after having fitted a polynomial to the spline
{
    // Check the boundaries
    if (x <= spline.front().first)
        return spline.front().second;
    if (x >= spline.back().first)
        return spline.back().second;
    //cerr << findExactSegment(hornerEvaluate(x), x) << endl;
    return selfInterpolate(findExactSegment(hornerEvaluate(x), x), x); 
}
//---------------------------------------------------------------------------
double SplineEvaluation::splittedChebyshevEvaluate(double& x) const 
// evaluates f(x), after having fitted a polynomial to the spline
{
    // Check the boundaries
    if (x <= spline.front().first)
        return spline.front().second;
    if (x >= spline.back().first)
        return spline.back().second;
    //cerr << findExactSegment(splittedHornerEvaluate(x), x) << endl;
    return selfInterpolate(findExactSegment(splittedHornerEvaluate(x), x), x);  
}
//---------------------------------------------------------------------------
double SplineEvaluation::btreeSegmentsEvaluate(double& x) const 
// evaluates f(x) with btree spline
{
    // Check the boundaries
    if (x <= spline.front().first)
        return spline.front().second;
    if (x >= spline.back().first)
        return spline.back().second;
    //cerr << "segment = " << (this->btreeSegments->lower_bound(x)->second) << "soll gleich " << realSegment(this->spline, x) << endl;
    auto iter = this->btreeSegments->lower_bound(x);
    return selfInterpolate(iter->second - 1, x);
}
//---------------------------------------------------------------------------
static double interpolate(const vector<Coord>& spline, double pos)
// evaluates the spline at pos
{
   if (pos<=spline.front().first)
      return spline.front().second;
   if (pos>=spline.back().first)
      return spline.back().second;
   auto iter=lower_bound(spline.begin(),spline.end(),pos,[](const Coord& a,double b) { return a.first<b; });
   if (iter->first==pos) 
       return iter->second;

   double dx=(iter+0)->first-(iter-1)->first;
   double dy=(iter+0)->second-(iter-1)->second;

   double ofs=pos-(iter-1)->first;
   return (iter-1)->second+ofs*(dy/dx);
}
//---------------------------------------------------------------------------
double SplineEvaluation::evaluate(double& pos) const 
// evaluates f(x)
{
#if 0
   if (pos<=spline.front().first)
      return spline.front().second;
   if (pos>=spline.back().first)
      return spline.back().second;
   bool now = pos < 0;
   auto iter=lower_bound(spline.begin()+now,spline.end()-now,pos,[](const Coord& a,double b) { return a.first<b; });
   if (iter->first==pos) 
       return iter->second;

   double dx=(iter+0)->first-(iter-1)->first;
   double dy=(iter+0)->second-(iter-1)->second;

   double ofs=pos-(iter-1)->first;
   return (iter-1)->second+ofs*(dy/dx);
#else
    return interpolate(this->spline, pos);
#endif
}
//---------------------------------------------------------------------------
double SplineEvaluation::mapEvaluate(double& pos) const 
// evaluate with the preprocessed map
{
    if (pos <= this->coordinates.begin()->first)
        return this->coordinates.begin()->second;
    if (pos >= (--this->coordinates.end())->first)
        return (--this->coordinates.end())->second;
    auto iter = this->coordinates.lower_bound(pos);
    if (iter->first == pos)
        return iter->second;
    return iter->second;
}
//---------------------------------------------------------------------------
double SplineEvaluation::btreeEvaluate(double& pos) const 
// evaluate with the preprocessed map
{
    if (pos <= this->btreeMap->begin()->first)
        return this->btreeMap->begin()->second;
    if (pos >= (--this->btreeMap->end())->first)
        return (--this->btreeMap->end())->second;
    auto iter = this->btreeMap->lower_bound(pos);
    return iter->second;
}
//---------------------------------------------------------------------------
int main(int argc,char* argv[])
{
   if (argc < 7) {
      cerr << "usage: " << argv[0] << " file samplingPoints countToRead(-1 -> all) useOfSpline(0 -> without Spline, 1 -> with Spline) modeOfSearch(0 -> split & fit, 1 -> only fit, 2 -> lower_bound, 3 -> map, 4 -> btree, 5 -> btree & spline) testMode(0, 1) fileRandom" << endl;
      return 1;
   }
#if 1
    // Read from a binary file the function
   unsigned countToRead = atoi(argv[3]);
   vector<Coord> cdf;
   {
        ifstream in(argv[1]);
        auto pos = in.tellg();
        in.seekg( 0, ios::end );  
        auto size=in.tellg()-pos;
        in.seekg(0,ios::beg);

        unsigned elements=size/sizeof(pair<double,double>);
        if ((countToRead != -1) && (countToRead < elements))
            elements = countToRead;
        cdf.resize(elements);
        in.read(reinterpret_cast<char*>(cdf.data()),elements*sizeof(Coord));
#if 0
        in.read(reinterpret_cast<char*>(cdf.data()),elements*sizeof(pair<double,double>));
        in.read(reinterpret_cast<char*>(cdf.data()),elements*sizeof(pair<double,double>));
#endif
    }
#else
    // Read from a .txt file
    unsigned countToRead = atoi(argv[3]);
    ifstream in(argv[1]);
    
    unsigned n;
    in >> n;
    vector<pair<double, double>> cdf(n);
    
    for (unsigned index = 0; index < n; ++index) {
        double x, y;
        in >> x >> y;
        cdf[index] = make_pair(x, y);
    }
#endif
    cerr << "Reading done!" << endl;

    unsigned size = atoi(argv[2]);
    unsigned useOfSpline = atoi(argv[4]);
    unsigned modeOfSearch = atoi(argv[5]);
    unsigned testMode = atoi(argv[6]);
    
    if (size > cdf.size()) {
        cerr << "Spline will contain more points than the function itself!" << endl;
        exit(0);
    }
    
    // Construct the spline and also fit the polynomial if modeSearch allows for that
    SplineEvaluation reduced(cdf, size, useOfSpline, modeOfSearch, testMode);

    // Shuffle the indexes in order to test
    std::vector<int> testIndexes(cdf.size());
    
    if (argc == 8) {
        ifstream randomFile(argv[7]);
        randomFile.read(reinterpret_cast<char*>(testIndexes.data()),cdf.size()*sizeof(int));
        randomFile.close();
    } else {
        for (unsigned index = 0; index < cdf.size(); ++index)
            testIndexes[index] = index;
        
        // This also has an influence upon the runtime
        unsigned seed = std::chrono::system_clock::now().time_since_epoch().count();
        shuffle(testIndexes.begin(), testIndexes.end(), std::default_random_engine(seed));
    }

    if (testMode == TEST_MODE_ACTIVATED) {
        // compare only those related to spline
        if (useOfSpline == WORK_ON_SPLINE_MODE) {
            cerr << "Start testing with spline!" << endl;
            double curr = 0;
            unsigned alright = 0;
            for (unsigned index = 0; index < cdf.size(); ++index) {
                Coord elem = cdf[testIndexes[index]];
                elem.first += 0.000;
                //cerr << setprecision(12) << "real segment of (" << elem.first << ", " << elem.second << ") is " << realSegment(reduced.spline, elem.first) << endl;  
                double y;
                if (modeOfSearch == SSE_SPLIT_MODE)
                    y = reduced.splittedChebyshevEvaluate(elem.first);
                else if (modeOfSearch == JUST_FIT_MODE)
                    y = reduced.chebyshevEvaluate(elem.first);
                else if (modeOfSearch == BTREE_SPLINE_MODE)
                    y = reduced.btreeSegmentsEvaluate(elem.first);
                else {
                    cerr << "Option in test with Spline not available " << modeOfSearch << endl; 
                    exit(0);
                }
                curr = max(curr, fabs(y - elem.second));
#if 1
                double cmp = reduced.evaluate(elem.first);
                if (!(fabs(cmp - y) < precision)) {
                    cerr << "Should be equal for " << setprecision(12) << fabs(cmp - y) << " " << elem.first << " --> " << setprecision(12) << y << " with " << setprecision(12)<<cmp << " against " << elem.second << endl;
                } else {
                    alright++;
                }
#endif
            }
            if (alright == cdf.size())
                cerr << "parfum!!!" << endl;
            else 
                cerr << "try again!!!Sth went wrong!!" << endl;
            cerr << setprecision(12) << "check: " << curr / cdf.size() << endl;
        } else {
            cerr << "Start testing without spline!" << endl;
            double curr = 0;
            unsigned alright = 0;
            for (unsigned index = 0; index < cdf.size(); ++index) {
                Coord elem = cdf[testIndexes[index]];
                elem.first += 0.005;
                //cerr << setprecision(12) << "real segment of (" << elem.first << ", " << elem.second << ") is " << realSegment(reduced.spline, elem.first) << endl;  
                double y;
                if (modeOfSearch == BTREE_MODE)
                    y = reduced.btreeEvaluate(elem.first);
                else {
                    cerr << "Option in test without Spline not available " << modeOfSearch << endl; 
                    exit(0);
                }
                curr = max(curr, fabs(y - elem.second));
#if 1
                double cmp = reduced.mapEvaluate(elem.first);
                if (!(fabs(cmp - y) < precision)) {
                    cerr << "Should be equal for " << setprecision(12) << fabs(cmp - y) << " " << elem.first << " --> " << setprecision(12) << y << " with " << setprecision(12)<<cmp << " against " << elem.second << endl;
                } else {
                    alright++;
                }
#endif
            }
            if (alright == cdf.size())
                cerr << "parfume!!!" << endl;
            else 
                cerr << "try again!!! Something went wrong!" << endl;
            cerr << setprecision(12) << "check: " << curr / cdf.size() << endl;
        }
    } else {
        auto start = std::chrono::high_resolution_clock::now();
        switch (modeOfSearch) {
            // use a split through SSE comparing, then fit
            case SSE_SPLIT_MODE : {
                double sum = 0, maxDiff = 0;
                for (unsigned index = 0; index < cdf.size(); ++index) {
#if 1
                    reduced.splittedChebyshevEvaluate(cdf[testIndexes[index]].first);
#else
                    // Test the deviations from the polynomial to the exact segment of each point
                    Coord elem = cdf[testIndexes[index]];
                    double segment = reduced.splittedHornerEvaluate(elem.first);
                    unsigned candSegment = filterSegment(reduced.spline.size() - 1, segment);
                    unsigned exactSeg = realSegment(reduced.spline, elem.first);
                    
                    double diff = (candSegment > exactSeg) ? (candSegment - exactSeg) : (exactSeg - candSegment);
                    sum += diff;
                    maxDiff = max(maxDiff, diff);
#endif
                }   
                cerr << "globalDiff = " << globalDiff << " mit " << globalDiff / cdf.size() << endl;
                cerr << "Avg deviation : " << (sum / cdf.size()) << " Max deviation : " << maxDiff << endl;
                break;
            }
            // Apply the fit of polynomial O(log(chebysevNorm))
            case JUST_FIT_MODE : {
                double sum = 0, maxDiff = 0;
                for (unsigned index = 0; index < cdf.size(); ++index) {
#if 1
                    reduced.chebyshevEvaluate(cdf[testIndexes[index]].first);
#else
                    // Test the deviations from the polynomial to the exact segment of each point
                    Coord elem = cdf[testIndexes[index]];
                    double segment = reduced.hornerEvaluate(elem.first);
                    unsigned candSegment = filterSegment(reduced.spline.size() - 1, segment);
                    unsigned exactSeg = realSegment(reduced.spline, elem.first);
                    
                    double diff = (candSegment > exactSeg) ? (candSegment - exactSeg) : (exactSeg - candSegment);
                    sum += diff;
                    maxDiff = max(maxDiff, diff);
#endif
                }
                cerr << "globalDiff = " << globalDiff << " mit " << globalDiff / cdf.size() << endl;
                cerr << "Avg deviation : " << (sum / cdf.size()) << " Max deviation : " << maxDiff << endl;
                break;
            }
            // Use only lower_bound O(log(size of spline))
            case LOWER_BOUND_MODE : {
                cerr << cdf.size() << endl;
                for (unsigned index = 0; index < cdf.size(); ++index) {
                    reduced.evaluate(cdf[testIndexes[index]].first);
                }
                break;
            }
            case MAP_MODE : {
                for (unsigned index = 0; index < cdf.size(); ++index) {
                    reduced.mapEvaluate(cdf[testIndexes[index]].first);
                }
                break;
            }
            case BTREE_MODE : {
                for (unsigned index = 0; index < cdf.size(); ++index) {
                    reduced.btreeEvaluate(cdf[testIndexes[index]].first);
                }
                break;
            }
            case BTREE_SPLINE_MODE : {
                for (unsigned index = 0; index < cdf.size(); ++index) {
                    reduced.btreeSegmentsEvaluate(cdf[testIndexes[index]].first);
                }
                break;
            }
            default : {
                cerr << "Choose again!" << endl;
            }
        }
        auto stop = std::chrono::high_resolution_clock::now();
        auto duration = stop - start;
        cerr << "modeOfSearch (0 -> split & fit, 1 -> only fit, 2 -> lower_bound, 3 -> map) : " << modeOfSearch << " took: " << duration.count() * 1e-6 << "ms" << endl;
    } // end of TEST_MODE_ACTIVATED
    return 0;
}
//---------------------------------------------------------------------------
