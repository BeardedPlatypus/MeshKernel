#include "MakeMeshes.cpp"
#include "../Entities.hpp"
#include "../Polygons.hpp"
#include "../Constants.cpp"
#include "../Mesh.hpp"
#include "../FlipEdges.cpp"

#include <gtest/gtest.h>

TEST(FlipEdges, FlipEdgesWithLandBoundary)
{
    //1 Setup
    auto mesh = MakeRectangularMeshForTesting(3, 3, 10, meshkernel::Projections::cartesian, { 0.0,0.0 });
    
    //set landboundaries
    auto polygon =std::make_shared<meshkernel::Polygons>();
    std::vector<meshkernel::Point> landBoundary{  {-1.369282, 21.249086},
                                                  { 20.885406, 21.539995},
                                                  {meshkernel::doubleMissingValue,meshkernel::doubleMissingValue} };

    auto landBoundaries = std::make_shared<meshkernel::LandBoundaries>(landBoundary, mesh, polygon);

    //execute flipedges
    meshkernel::FlipEdges flipEdges(mesh, landBoundaries, true, true);

    auto successful = flipEdges.Compute();
    ASSERT_TRUE(successful);

    // check the values
    ASSERT_EQ(16, mesh->GetNumEdges());
}

TEST(FlipEdges, FlipEdgesMediumTriangularMesh)
{
    //1 Setup
    auto mesh = MakeMediumSizeTriangularMeshForTesting();

    //set landboundaries
    auto polygon =std::make_shared<meshkernel::Polygons>();

    std::vector<meshkernel::Point> landBoundary;
    auto landBoundaries = std::make_shared<meshkernel::LandBoundaries>(landBoundary, mesh, polygon);

    //execute flipedges
    meshkernel::FlipEdges flipEdges(mesh, landBoundaries, true, false);

    auto successful = flipEdges.Compute();
    ASSERT_TRUE(successful);

     // get the number of edges
    ASSERT_EQ(697, mesh->GetNumEdges());

    // check the values of flipped edges
    ASSERT_EQ(183, mesh->m_edges[14].first);
    ASSERT_EQ(227, mesh->m_edges[14].second);

    ASSERT_EQ(58, mesh->m_edges[33].first);
    ASSERT_EQ(141, mesh->m_edges[33].second);

    ASSERT_EQ(147, mesh->m_edges[46].first);
    ASSERT_EQ(145, mesh->m_edges[46].second);

    ASSERT_EQ(147, mesh->m_edges[49].first);
    ASSERT_EQ(148, mesh->m_edges[49].second);

    ASSERT_EQ(242, mesh->m_edges[68].first);
    ASSERT_EQ(148, mesh->m_edges[68].second);

}