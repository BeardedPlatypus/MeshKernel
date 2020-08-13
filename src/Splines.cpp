#pragma once

#include <vector>
#include <algorithm>
#include <cassert>
#include "Operations.cpp"
#include "Entities.hpp"
#include "CurvilinearParametersNative.hpp"
#include "SplinesToCurvilinearParametersNative.hpp"
#include "Splines.hpp"
#include "CurvilinearGrid.hpp"

GridGeom::Splines::Splines() : m_projection(Projections::cartesian)
{
};

GridGeom::Splines::Splines(Projections projection) : m_projection(projection)
{
};

/// add a new spline, return the index
bool GridGeom::Splines::AddSpline(const std::vector<Point>& splines, int start, int size)
{
    ResizeVectorIfNeededWithMinimumSize(m_numSplines + 1, m_splineCornerPoints, m_allocationSize, std::vector<Point>(10, { doubleMissingValue, doubleMissingValue }));

    m_numAllocatedSplines = m_splineCornerPoints.size();
    m_numAllocatedSplineNodes.resize(m_numAllocatedSplines, 10);

    m_numSplineNodes.resize(m_numAllocatedSplines, 0);
    m_numSplineNodes[m_numSplines] = size;

    m_splineDerivatives.resize(m_numAllocatedSplines);
    int index = 0;
    for (int n = start; n < start + size; ++n)
    {
        m_splineCornerPoints[m_numSplines][index] = splines[n];
        index++;
    }
    m_splinesLength.resize(m_numAllocatedSplines);

    // compute basic properties
    SecondOrderDerivative(m_splineCornerPoints[m_numSplines], m_numSplineNodes[m_numSplines], m_splineDerivatives[m_numSplines]);
    m_splinesLength[m_numSplines] = GetSplineLength(m_numSplines, 0, m_numSplineNodes[m_numSplines] - 1);
    m_numSplines++;

    return true;
}

bool GridGeom::Splines::DeleteSpline(int splineIndex)
{
    m_splineCornerPoints.erase(m_splineCornerPoints.begin() + splineIndex);
    m_numSplineNodes.erase(m_numSplineNodes.begin() + splineIndex);
    m_splineDerivatives.erase(m_splineDerivatives.begin() + splineIndex);
    m_splinesLength.erase(m_splinesLength.begin() + splineIndex);
    m_numSplines--;
    return true;
}

/// add a new spline point in an existing spline
bool GridGeom::Splines::AddPointInExistingSpline(int splineIndex, const Point& point)
{
    if (splineIndex > m_numSplines)
    {
        return false;
    }
    ResizeVectorIfNeededWithMinimumSize(m_numSplineNodes[splineIndex] + 1, m_splineCornerPoints[splineIndex], m_allocationSize, { doubleMissingValue, doubleMissingValue });
    m_numAllocatedSplineNodes[splineIndex] = m_splineCornerPoints[splineIndex].size();

    m_splineCornerPoints[splineIndex][m_numSplineNodes[splineIndex]] = point;
    m_numSplineNodes[splineIndex]++;
    return true;
}

bool GridGeom::Splines::GetSplinesIntersection(const int first, const int second,
    const Projections& projection,
    double& crossProductIntersection,
    Point& intersectionPoint,
    double& firstSplineRatio,
    double& secondSplineRatio)
{
    double minimumCrossingDistance = std::numeric_limits<double>::max();
    double crossingDistance;
    int numCrossing = 0;
    double firstCrossingRatio;
    double secondCrossingRatio;
    int firstCrossingIndex = 0;
    int secondCrossingIndex = 0;
    Point closestIntersection;

    // First find a valid crossing, the closest to spline central point
    for (int n = 0; n < m_numSplineNodes[first] - 1; n++)
    {
        for (int nn = 0; nn < m_numSplineNodes[second] - 1; nn++)
        {
            Point intersection;
            double crossProduct;
            double firstRatio;
            double secondRatio;
            bool areCrossing = AreLinesCrossing(m_splineCornerPoints[first][n],
                m_splineCornerPoints[first][n + 1],
                m_splineCornerPoints[second][nn],
                m_splineCornerPoints[second][nn + 1],
                false,
                intersection,
                crossProduct,
                firstRatio,
                secondRatio,
                projection);


            if (areCrossing)
            {
                if (m_numSplineNodes[first] == 2)
                {
                    crossingDistance = std::min(minimumCrossingDistance, std::abs(firstRatio - 0.5));
                }
                else if (m_numSplineNodes[second] == 2)
                {
                    crossingDistance = std::abs(secondRatio - 0.5);
                }
                else
                {
                    crossingDistance = minimumCrossingDistance;
                }

                if (crossingDistance < minimumCrossingDistance || numCrossing == 0)
                {
                    minimumCrossingDistance = crossingDistance;
                    numCrossing = 1;
                    firstCrossingIndex = n;             //TI0
                    secondCrossingIndex = nn;           //TJ0
                    firstCrossingRatio = firstRatio;    //SL
                    secondCrossingRatio = secondRatio;  //SM
                }
            }
            closestIntersection = intersection;
        }
    }

    // if no crossing found, return
    if (numCrossing == 0)
    {
        return false;
    }

    double firstCrossing = firstCrossingRatio == -1 ? 0 : firstCrossingIndex + firstCrossingRatio;
    double secondCrossing = secondCrossingRatio == -1 ? 0 : secondCrossingIndex + secondCrossingRatio;

    // use bisection to find the intersection 
    double squaredDistanceBetweenCrossings = std::numeric_limits<double>::max();
    double maxSquaredDistanceBetweenCrossings = 1e-12;
    double maxDistanceBetweenVertices = 0.0001;
    double firstRatioIterations = 1.0;
    double secondRatioIterations = 1.0;
    double previousFirstCrossing;
    double previousSecondCrossing;
    int numIterations = 0;
    while (squaredDistanceBetweenCrossings > maxSquaredDistanceBetweenCrossings&& numIterations < 20)
    {
        // increment counter
        numIterations++;

        if (firstCrossingRatio > 0 && firstCrossingRatio < 1.0)
        {
            firstRatioIterations = 0.5 * firstRatioIterations;
        }
        if (secondCrossingRatio > 0 && secondCrossingRatio < 1.0)
        {
            secondRatioIterations = 0.5 * secondRatioIterations;
        }

        firstCrossing = std::max(0.0, std::min(firstCrossing, double(m_numSplineNodes[first])));
        secondCrossing = std::max(0.0, std::min(secondCrossing, double(m_numSplineNodes[second])));

        double firstLeft = std::max(0.0, std::min(double(m_numSplineNodes[first] - 1), firstCrossing - firstRatioIterations / 2.0));
        double firstRight = std::max(0.0, std::min(double(m_numSplineNodes[first] - 1), firstCrossing + firstRatioIterations / 2.0));

        double secondLeft = std::max(0.0, std::min(double(m_numSplineNodes[second] - 1), secondCrossing - secondRatioIterations / 2.0));
        double secondRight = std::max(0.0, std::min(double(m_numSplineNodes[second] - 1), secondCrossing + secondRatioIterations / 2.0));

        firstRatioIterations = firstRight - firstLeft;
        secondRatioIterations = secondRight - secondLeft;

        Point firstLeftSplinePoint;
        InterpolateSplinePoint( m_splineCornerPoints[first], 
                     m_splineDerivatives[first], 
                     firstLeft, 
                     firstLeftSplinePoint);
        Point firstRightSplinePoint;
        InterpolateSplinePoint(m_splineCornerPoints[first], m_splineDerivatives[first], firstRight, firstRightSplinePoint);

        Point secondLeftSplinePoint;
        InterpolateSplinePoint(m_splineCornerPoints[second], m_splineDerivatives[second], secondLeft, secondLeftSplinePoint);
        Point secondRightSplinePoint;
        InterpolateSplinePoint(m_splineCornerPoints[second], m_splineDerivatives[second], secondRight, secondRightSplinePoint);

        Point oldIntersection = closestIntersection;

        double crossProduct;
        double firstRatio;
        double secondRatio;
        bool areCrossing = AreLinesCrossing(firstLeftSplinePoint, firstRightSplinePoint,
            secondLeftSplinePoint, secondRightSplinePoint,
            true,
            closestIntersection,
            crossProduct,
            firstRatio,
            secondRatio,
            projection);

        // search close by
        if (-2.0 < firstRatio < 3.0 && -2.0 < secondRatio < 3.0)
        {
            previousFirstCrossing = firstCrossing;
            previousSecondCrossing = secondCrossing;

            firstCrossing = firstLeft + firstRatio * (firstRight - firstLeft);
            secondCrossing = secondLeft + secondRatio * (secondRight - secondLeft);

            firstCrossing = std::max(0.0, std::min(m_numSplineNodes[first] - 1.0, firstCrossing));
            secondCrossing = std::max(0.0, std::min(m_numSplineNodes[second] - 1.0, secondCrossing));

            if (areCrossing)
            {
                numCrossing = 1;
                crossProductIntersection = crossProduct;
            }

            if (std::abs(firstCrossing - previousFirstCrossing) > maxDistanceBetweenVertices ||
                std::abs(secondCrossing - previousSecondCrossing) > maxDistanceBetweenVertices)
            {
                squaredDistanceBetweenCrossings = ComputeSquaredDistance(oldIntersection, closestIntersection, projection);
            }
            else
            {
                break;
            }
        }
    }

    if (numCrossing == 1)
    {
        intersectionPoint = closestIntersection;
        firstSplineRatio = firstCrossing;
        secondSplineRatio = secondCrossing;
        return true;
    }

    //not crossing
    return false;
}

double GridGeom::Splines::GetSplineLength(int index,
    double beginFactor,
    double endFactor,
    int numSamples,
    bool accountForCurvature,
    double height,
    double assignedDelta)
{
    double delta = assignedDelta;
    int numPoints = endFactor / delta + 1;
    if (delta < 0.0)
    {
        delta = 1.0 / numSamples;
        numPoints = std::max(std::floor(0.9999 + (endFactor - beginFactor) / delta), 10.0);
        delta = (endFactor - beginFactor) / numPoints;
    }

    // first point
    Point leftPoint;
    InterpolateSplinePoint(m_splineCornerPoints[index], m_splineDerivatives[index], beginFactor, leftPoint);

    double splineLength = 0.0;

    double rightPointCoordinateOnSpline = beginFactor;
    double leftPointCoordinateOnSpline;
    for (int p = 0; p < numPoints; ++p)
    {
        leftPointCoordinateOnSpline = rightPointCoordinateOnSpline;
        rightPointCoordinateOnSpline += delta;
        if (rightPointCoordinateOnSpline > endFactor)
        {
            rightPointCoordinateOnSpline = endFactor;
        }

        Point rightPoint;
        InterpolateSplinePoint(m_splineCornerPoints[index], m_splineDerivatives[index], rightPointCoordinateOnSpline, rightPoint);
        double curvatureFactor = 0.0;
        if (accountForCurvature)
        {
            Point normalVector;
            Point tangentialVector;
            ComputeCurvatureOnSplinePoint(index, 0.5 * (rightPointCoordinateOnSpline + leftPointCoordinateOnSpline), curvatureFactor, normalVector, tangentialVector);
        }
        splineLength = splineLength + Distance(leftPoint, rightPoint, m_projection) * (1.0 + curvatureFactor * height);
        leftPoint = rightPoint;
    }

    return splineLength;
}

bool GridGeom::Splines::ComputeCurvatureOnSplinePoint( int splineIndex,
                                                       double adimensionalPointCoordinate,
                                                       double& curvatureFactor,
                                                       Point& normalVector,
                                                       Point& tangentialVector)
{
    auto const leftCornerPoint = int(std::max(std::min(double(std::floor(adimensionalPointCoordinate)), double(m_numSplineNodes[splineIndex] - 1)), 0.0));
    auto const rightCornerPoint = int(std::max(double(leftCornerPoint + 1.0), 0.0));

    double leftSegment = rightCornerPoint - adimensionalPointCoordinate;
    double rightSegment = adimensionalPointCoordinate - leftCornerPoint;

    Point pointCoordinate;
    InterpolateSplinePoint(m_splineCornerPoints[splineIndex], m_splineDerivatives[splineIndex], adimensionalPointCoordinate, pointCoordinate);

    Point p = m_splineCornerPoints[splineIndex][rightCornerPoint] - m_splineCornerPoints[splineIndex][leftCornerPoint] + 
              (m_splineDerivatives[splineIndex][leftCornerPoint] * (-3.0 * leftSegment * leftSegment + 1.0) + 
               m_splineDerivatives[splineIndex][rightCornerPoint] * (3.0 * rightSegment * rightSegment - 1.0)) / 6.0;

    Point pp = m_splineDerivatives[splineIndex][leftCornerPoint] * leftSegment + 
               m_splineDerivatives[splineIndex][rightCornerPoint] * rightSegment;

    if (m_projection == Projections::spherical)
    {
        p.TransformSphericalToCartesian(pointCoordinate.y);
        pp.TransformSphericalToCartesian(pointCoordinate.y);
    }

    curvatureFactor = std::abs(pp.x * p.y - pp.y * p.x) / std::pow((p.x * p.x + p.y * p.y + 1e-8), 1.5);

    Point incremenetedPointCoordinate = pointCoordinate + p * 1e-4;
    NormalVectorOutside(pointCoordinate, incremenetedPointCoordinate, normalVector, m_projection);

    double distance = Distance(pointCoordinate, incremenetedPointCoordinate, m_projection);
    double dx = GetDx(pointCoordinate, incremenetedPointCoordinate, m_projection);
    double dy = GetDy(pointCoordinate, incremenetedPointCoordinate, m_projection);

    tangentialVector.x = dx / distance;
    tangentialVector.y = dy / distance;

    return true;
}

bool GridGeom::Splines::SecondOrderDerivative(const std::vector<Point>& coordinates, int numNodes, std::vector<Point>& coordinatesDerivatives)
{
    std::vector<Point> u(numNodes);
    u[0] = { 0.0, 0.0 };
    coordinatesDerivatives.resize(coordinates.size(), { 0.0, 0.0 });
    coordinatesDerivatives[0] = { 0.0, 0.0 };

    for (int i = 1; i < numNodes - 1; i++)
    {
        const Point p = coordinatesDerivatives[i - 1] * 0.5 + 2.0;
        coordinatesDerivatives[i].x = -0.5 / p.x;
        coordinatesDerivatives[i].y = -0.5 / p.y;

        const Point delta = coordinates[i + 1] - coordinates[i] - (coordinates[i] - coordinates[i - 1]);
        u[i] = (delta * 6.0 / 2.0 - u[i - 1] * 0.5) / p;
    }

    coordinatesDerivatives[numNodes - 1] = { 0.0, 0.0 };
    for (int i = numNodes - 2; i >= 0; i--)
    {
        coordinatesDerivatives[i] = coordinatesDerivatives[i] * coordinatesDerivatives[i + 1] + u[i];
    }

    return true;
}

bool GridGeom::Splines::SecondOrderDerivative(const std::vector<double>& coordinates, int numNodes, std::vector<double>& coordinatesDerivatives)
{
    std::vector<double> u(numNodes);
    u[0] = 0.0;
    coordinatesDerivatives[0] = 0.0;

    for (int i = 1; i < numNodes - 1; i++)
    {
        const double p = coordinatesDerivatives[i - 1] * 0.5 + 2.0;
        coordinatesDerivatives[i] = -0.5 / p;

        const double delta = coordinates[i + 1] - coordinates[i] - (coordinates[i] - coordinates[i - 1]);
        u[i] = (delta * 6.0 / 2.0 - u[i - 1] * 0.5) / p;
    }

    coordinatesDerivatives[numNodes - 1] = 0.0;
    for (int i = numNodes - 2; i >= 0; i--)
    {
        coordinatesDerivatives[i] = coordinatesDerivatives[i] * coordinatesDerivatives[i + 1] + u[i];
    }

    return true;
}