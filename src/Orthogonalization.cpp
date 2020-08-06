#include <vector>
#include <algorithm>
#include <numeric>
#include <string>

#include "Operations.cpp"
#include "Orthogonalization.hpp"
#include "Entities.hpp"
#include "Mesh.hpp"

bool GridGeom::Orthogonalization::Set(Mesh& mesh,
    int& isTriangulationRequired,
    int& isAccountingForLandBoundariesRequired,
    int& projectToLandBoundaryOption,
    GridGeomApi::OrthogonalizationParametersNative& orthogonalizationParametersNative,
    const Polygons& polygon,
    std::vector<Point>& landBoundaries)
{
    m_maxNumNeighbours = *(std::max_element(mesh.m_nodesNumEdges.begin(), mesh.m_nodesNumEdges.end()));
    m_maxNumNeighbours += 1;
    m_nodesNodes.resize(mesh.GetNumNodes() , std::vector<int>(m_maxNumNeighbours, intMissingValue));
    m_wOrthogonalizer.resize(mesh.GetNumNodes() , std::vector<double>(m_maxNumNeighbours, 0.0));
    m_rhsOrthogonalizer.resize(mesh.GetNumNodes() , std::vector<double>(2, 0.0));
    m_aspectRatios.resize(mesh.GetNumEdges(), 0.0);
    m_polygons = polygon;

    // Sets the node mask
    mesh.MaskNodesInPolygons(m_polygons, true);
    // Flag nodes outside the polygon as corner points
    for (auto n = 0; n < mesh.GetNumNodes(); n++)
    {
        if (mesh.m_nodeMask[n] == 0) 
        {
            mesh.m_nodesTypes[n] = 3;
        }
    }

    //for each node, determine the neighbouring nodes
    for (auto n = 0; n < mesh.GetNumNodes() ; n++)
    {
        for (auto nn = 0; nn < mesh.m_nodesNumEdges[n]; nn++)
        {
            Edge edge = mesh.m_edges[mesh.m_nodesEdges[n][nn]];
            m_nodesNodes[n][nn] = edge.first + edge.second - n;
        }
    }

    // TODO: calculate volume weights for areal smoother
    m_mumax = (1.0 - m_smoothorarea) * 0.5;
    m_mu = std::min(1e-2, m_mumax);
    m_orthogonalCoordinates.resize(mesh.GetNumNodes() );

    // in this case the nearest point is the point itself
    m_nearestPoints.resize(mesh.GetNumNodes() );
    std::iota(m_nearestPoints.begin(), m_nearestPoints.end(), 0);

    // back-up original nodes, for projection on original mesh boundary
    m_originalNodes = mesh.m_nodes;
    m_orthogonalCoordinates = mesh.m_nodes;

    // algorithm settings
    m_orthogonalizationToSmoothingFactor = orthogonalizationParametersNative.OrthogonalizationToSmoothingFactor;
    m_orthogonalizationToSmoothingFactorBoundary = orthogonalizationParametersNative.OrthogonalizationToSmoothingFactorBoundary;
    m_smoothorarea = orthogonalizationParametersNative.Smoothorarea;
    m_orthogonalizationOuterIterations = orthogonalizationParametersNative.OuterIterations;
    m_orthogonalizationBoundaryIterations = orthogonalizationParametersNative.BoundaryIterations;
    m_orthogonalizationInnerIterations = orthogonalizationParametersNative.InnerIterations;

    m_landBoundaries.Set(landBoundaries);

    m_isTriangulationRequired = isTriangulationRequired;

    m_isAccountingForLandBoundariesRequired = isAccountingForLandBoundariesRequired;

    m_projectToLandBoundaryOption = projectToLandBoundaryOption;

    // project on land boundary
    if (m_projectToLandBoundaryOption >= 1)
    {
        // account for enclosing polygon
        m_landBoundaries.Administrate(mesh, m_polygons);
        m_landBoundaries.FindNearestMeshBoundary(mesh, m_polygons, m_projectToLandBoundaryOption);
    }

    // for spherical accurate computations we need to call orthonet_comp_ops 
    if(mesh.m_projection == Projections::sphericalAccurate)
    {
        if(m_orthogonalizationToSmoothingFactor<1.0)
        {
            bool successful = PrapareOuterIteration(mesh);
            if (!successful)
            {
                return false;
            }
        }

        m_localCoordinatesIndexes.resize(mesh.GetNumNodes() + 1);
        m_localCoordinatesIndexes[0] = 1;
        for (int n = 0; n < mesh.GetNumNodes(); ++n)
        {
            m_localCoordinatesIndexes[n + 1] = m_localCoordinatesIndexes[n] + std::max(mesh.m_nodesNumEdges[n] + 1, m_numConnectedNodes[n]);
        }

        m_localCoordinates.resize(m_localCoordinatesIndexes.back() - 1, { doubleMissingValue, doubleMissingValue });
    }

    return true;
}

bool GridGeom::Orthogonalization::Compute(Mesh& mesh)
{
    bool successful = true;

    for (auto outerIter = 0; outerIter < m_orthogonalizationOuterIterations; outerIter++)
    {
        if (successful)
        {
            successful = PrapareOuterIteration(mesh);
        }
        for (auto boundaryIter = 0; boundaryIter < m_orthogonalizationBoundaryIterations; boundaryIter++)
        {
            for (auto innerIter = 0; innerIter < m_orthogonalizationInnerIterations; innerIter++)
            {
                if (successful)
                {
                    successful = InnerIteration(mesh);
                }        
            } // inner iteration
        } // boundary iter

        //update mu
        if (successful)
        {
            successful = FinalizeOuterIteration(mesh);
        }    
    }// outer iter

    DeallocateLinearSystem();

    return true;
}


bool GridGeom::Orthogonalization::PrapareOuterIteration(const Mesh& mesh) 
{

    bool successful = true;

    //compute aspect ratios
    if (successful)
    {
        successful = AspectRatio(mesh);
    }

    //compute weights and rhs of orthogonalizer
    if (successful)
    {
        successful = ComputeWeightsAndRhsOrthogonalizer(mesh);
    }

    // compute local coordinates
    if (successful)
    {
        successful = ComputeLocalCoordinates(mesh);
    }

    // compute smoother topologies
    if (successful)
    {
        successful = ComputeSmootherTopologies(mesh);
    }

    // compute smoother topologies
    if (successful)
    {
        successful = ComputeSmootherOperators(mesh);
    }

    // compute weights smoother
    if (successful)
    {
        successful = ComputeSmootherWeights(mesh);
    }
    
    // allocate linear system for smoother and orthogonalizer
    if (successful)
    {
        successful = AllocateLinearSystem(mesh);
    }

    // compute linear system terms for smoother and orthogonalizer
    if (successful)
    {
        successful = ComputeLinearSystemTerms(mesh);
    }
    return successful;
}


bool GridGeom::Orthogonalization::AllocateLinearSystem(const Mesh& mesh) 
{
    bool successful = true;
    // reallocate caches
    if (successful && m_nodeCacheSize == 0)
    {
        m_compressedRhs.resize(mesh.GetNumNodes() * 2);
        std::fill(m_compressedRhs.begin(), m_compressedRhs.end(), 0.0);

        m_compressedEndNodeIndex.resize(mesh.GetNumNodes());
        std::fill(m_compressedEndNodeIndex.begin(), m_compressedEndNodeIndex.end(), 0.0);

        m_compressedStartNodeIndex.resize(mesh.GetNumNodes() );
        std::fill(m_compressedStartNodeIndex.begin(), m_compressedStartNodeIndex.end(), 0.0);

        for (int n = 0; n < mesh.GetNumNodes() ; n++)
        {
            m_compressedEndNodeIndex[n] = m_nodeCacheSize;
            m_nodeCacheSize += std::max(mesh.m_nodesNumEdges[n] + 1, m_numConnectedNodes[n]);
            m_compressedStartNodeIndex[n] = m_nodeCacheSize;
        }

        m_compressedNodesNodes.resize(m_nodeCacheSize);
        m_compressedWeightX.resize(m_nodeCacheSize);
        m_compressedWeightY.resize(m_nodeCacheSize);
    }
    return successful;
}


bool GridGeom::Orthogonalization::DeallocateLinearSystem() 
{
    m_compressedRhs.resize(0);
    m_compressedEndNodeIndex.resize(0);
    m_compressedStartNodeIndex.resize(0);
    m_compressedNodesNodes.resize(0);
    m_compressedWeightX.resize(0);
    m_compressedWeightY.resize(0);
    m_nodeCacheSize = 0;

    return true;
}

bool GridGeom::Orthogonalization::FinalizeOuterIteration(Mesh& mesh)
{
    m_mu = std::min(2.0 * m_mu, m_mumax);

    //compute new faces circumcenters
    if (!m_keepCircumcentersAndMassCenters)
    {
        mesh.ComputeFaceCircumcentersMassCentersAndAreas();
    }

    return true;
}

bool GridGeom::Orthogonalization::ComputeLinearSystemTerms(const Mesh& mesh)
{
    double max_aptf = std::max(m_orthogonalizationToSmoothingFactorBoundary, m_orthogonalizationToSmoothingFactor);
#pragma omp parallel for
	for (int n = 0; n < mesh.GetNumNodes() ; n++)
    {    
        if ((mesh.m_nodesTypes[n] != 1 && mesh.m_nodesTypes[n] != 2) || mesh.m_nodesNumEdges[n] < 2)
        {
            continue;
        }
        if (m_keepCircumcentersAndMassCenters != false && (mesh.m_nodesNumEdges[n] != 3 || mesh.m_nodesNumEdges[n] != 1))
        {
            continue;
        }

        const double atpfLoc  = mesh.m_nodesTypes[n] == 2 ? max_aptf : m_orthogonalizationToSmoothingFactor;
        const double atpf1Loc = 1.0 - atpfLoc;
        double mumat    = m_mu;
        int maxnn = m_compressedStartNodeIndex[n] - m_compressedEndNodeIndex[n];
        for (int nn = 1, cacheIndex = m_compressedEndNodeIndex[n]; nn < maxnn; nn++, cacheIndex++)
        {
            double wwx = 0.0;
            double wwy = 0.0;

            // Smoother
            if (atpf1Loc > 0.0 && mesh.m_nodesTypes[n] == 1)
            {
                wwx = atpf1Loc * m_wSmoother[n][nn];
                wwy = atpf1Loc * m_wSmoother[n][nn];
            }
            
            // Orthogonalizer
            if (nn < mesh.m_nodesNumEdges[n] + 1)
            {
                wwx += atpfLoc * m_wOrthogonalizer[n][nn - 1];
                wwy += atpfLoc * m_wOrthogonalizer[n][nn - 1];
                m_compressedNodesNodes[cacheIndex] = m_nodesNodes[n][nn - 1];
            }
            else
            {
                m_compressedNodesNodes[cacheIndex] = m_connectedNodes[n][nn];
            }

            m_compressedWeightX[cacheIndex] = wwx;
            m_compressedWeightY[cacheIndex] = wwy;
        }
        int firstCacheIndex = n * 2;
        m_compressedRhs[firstCacheIndex] = atpfLoc * m_rhsOrthogonalizer[n][0];
        m_compressedRhs[firstCacheIndex+1] = atpfLoc * m_rhsOrthogonalizer[n][1];
	}

	return true;
}


bool GridGeom::Orthogonalization::InnerIteration(Mesh& mesh)
{
#pragma omp parallel for
	for (int n = 0; n < mesh.GetNumNodes() ; n++)
    {
	    UpdateNodeCoordinates(n, mesh);   
    }
	
    // update mesh node coordinates
    mesh.m_nodes = m_orthogonalCoordinates;

    // project on the original net boundary
    ProjectOnOriginalMeshBoundary(mesh);

    // compute local coordinates
    ComputeLocalCoordinates(mesh);

    // project on land boundary
    if (m_projectToLandBoundaryOption >= 1)
    {
        m_landBoundaries.SnapMeshToLandBoundaries(mesh);
    }

    return true;
}

bool GridGeom::Orthogonalization::ProjectOnOriginalMeshBoundary(Mesh& mesh)
{
    Point firstPoint;
    Point secondPoint;
    Point thirdPoint;
    Point normalSecondPoint;
    Point normalThirdPoint;

    for (auto n = 0; n < mesh.GetNumNodes() ; n++)
    {
        int nearestPointIndex = m_nearestPoints[n];
        if (mesh.m_nodesTypes[n] == 2 && mesh.m_nodesNumEdges[n] > 0 && mesh.m_nodesNumEdges[nearestPointIndex] > 0)
        {
            firstPoint = mesh.m_nodes[n];
            int numEdges = mesh.m_nodesNumEdges[nearestPointIndex];
            int numNodes = 0;
            int leftNode;
            int rightNode;
            for (auto nn = 0; nn < numEdges; nn++)
            {
                auto edgeIndex = mesh.m_nodesEdges[nearestPointIndex][nn];
                if (mesh.m_edgesNumFaces[edgeIndex] == 1)
                {
                    numNodes++;
                    if (numNodes == 1)
                    {
                        leftNode = m_nodesNodes[n][nn];
                        if (leftNode == intMissingValue)
                        {
                            return false;
                        }
                        secondPoint = m_originalNodes[leftNode];
                    }
                    else if (numNodes == 2)
                    {
                        rightNode = m_nodesNodes[n][nn];
                        if (rightNode == intMissingValue)
                        {
                            return false;
                        }
                        thirdPoint = m_originalNodes[rightNode];
                    }
                }
            }

            //Project the moved boundary point back onto the closest original edge (either between 0 and 2 or 0 and 3)
            double rl2;
            double dis2 = DistanceFromLine(firstPoint, m_originalNodes[nearestPointIndex], secondPoint, normalSecondPoint, rl2, mesh.m_projection);

            double rl3;
            double dis3 = DistanceFromLine(firstPoint, m_originalNodes[nearestPointIndex], thirdPoint, normalThirdPoint, rl3, mesh.m_projection);

            if (dis2 < dis3)
            {
                mesh.m_nodes[n] = normalSecondPoint;
                if (rl2 > 0.5 && mesh.m_nodesTypes[n] != 3)
                {
                    m_nearestPoints[n] = leftNode;
                }
            }
            else
            {
                mesh.m_nodes[n] = normalThirdPoint;
                if (rl3 > 0.5 && mesh.m_nodesTypes[n] != 3)
                {
                    m_nearestPoints[n] = rightNode;
                }
            }
        }
    }
    return true;
}


bool GridGeom::Orthogonalization::ComputeSmootherWeights(const Mesh& mesh)
{
    std::vector<std::vector<double>> J(mesh.GetNumNodes() , std::vector<double>(4, 0));    // Jacobian
    std::vector<std::vector<double>> Ginv(mesh.GetNumNodes() , std::vector<double>(4, 0)); // Mesh monitor matrices

    for (auto n = 0; n < mesh.GetNumNodes() ; n++)
    {
        if (mesh.m_nodesTypes[n] != 1 && mesh.m_nodesTypes[n] != 2 && mesh.m_nodesTypes[n] != 4) 
        {
            continue;
        }
        ComputeJacobian(n, mesh, J[n]);
    }

    // TODO: Account for samples: call orthonet_comp_Ginv(u, ops, J, Ginv)
    for (auto n = 0; n < mesh.GetNumNodes() ; n++)
    {
        Ginv[n][0] = 1.0;
        Ginv[n][1] = 0.0;
        Ginv[n][2] = 0.0;
        Ginv[n][3] = 1.0;
    }

    m_wSmoother.resize(mesh.GetNumNodes() , std::vector<double>(m_maximumNumConnectedNodes, 0));
    std::vector<double> a1(2);
    std::vector<double> a2(2);

    // matrices for dicretization
    std::vector<double> DGinvDxi(4, 0.0);
    std::vector<double> DGinvDeta(4, 0.0);
    std::vector<double> currentGinv(4, 0.0);
    std::vector<double> GxiByDivxi(m_maximumNumConnectedNodes, 0.0);
    std::vector<double> GxiByDiveta(m_maximumNumConnectedNodes, 0.0);
    std::vector<double> GetaByDivxi(m_maximumNumConnectedNodes, 0.0);
    std::vector<double> GetaByDiveta(m_maximumNumConnectedNodes, 0.0);
    for (auto n = 0; n < mesh.GetNumNodes() ; n++)
    {

        if (mesh.m_nodesNumEdges[n] < 2) continue;

        // Internal nodes and boundary nodes
        if (mesh.m_nodesTypes[n] == 1 || mesh.m_nodesTypes[n] == 2)
        {
            int currentTopology = m_nodeTopologyMapping[n];

            ComputeJacobian(n, mesh, J[n]);

            //compute the contravariant base vectors
            double determinant = J[n][0] * J[n][3] - J[n][3] * J[n][1];
            if (determinant == 0.0)
            {
                continue;
            }

            a1[0] = J[n][3] / determinant;
            a1[1] = -J[n][2] / determinant;
            a2[0] = -J[n][1] / determinant;
            a2[1] = J[n][0] / determinant;

            std::fill(DGinvDxi.begin(), DGinvDxi.end(), 0.0);
            std::fill(DGinvDeta.begin(), DGinvDeta.end(), 0.0);
            for (int i = 0; i < m_numTopologyNodes[currentTopology]; i++)
            {
                DGinvDxi[0] += Ginv[m_topologyConnectedNodes[currentTopology][i]][0] * m_Jxi[currentTopology][i];
                DGinvDxi[1] += Ginv[m_topologyConnectedNodes[currentTopology][i]][1] * m_Jxi[currentTopology][i];
                DGinvDxi[2] += Ginv[m_topologyConnectedNodes[currentTopology][i]][2] * m_Jxi[currentTopology][i];
                DGinvDxi[3] += Ginv[m_topologyConnectedNodes[currentTopology][i]][3] * m_Jxi[currentTopology][i];

                DGinvDeta[0] += Ginv[m_topologyConnectedNodes[currentTopology][i]][0] * m_Jeta[currentTopology][i];
                DGinvDeta[1] += Ginv[m_topologyConnectedNodes[currentTopology][i]][1] * m_Jeta[currentTopology][i];
                DGinvDeta[2] += Ginv[m_topologyConnectedNodes[currentTopology][i]][2] * m_Jeta[currentTopology][i];
                DGinvDeta[3] += Ginv[m_topologyConnectedNodes[currentTopology][i]][3] * m_Jeta[currentTopology][i];
            }

            // compute current Ginv
            currentGinv[0] = Ginv[n][0];
            currentGinv[1] = Ginv[n][1];
            currentGinv[2] = Ginv[n][2];
            currentGinv[3] = Ginv[n][3];

            // compute small matrix operations
            std::fill(GxiByDivxi.begin(), GxiByDivxi.end(), 0.0);
            std::fill(GxiByDiveta.begin(), GxiByDiveta.end(), 0.0);
            std::fill(GetaByDivxi.begin(), GetaByDivxi.end(), 0.0);
            std::fill(GetaByDiveta.begin(), GetaByDiveta.end(), 0.0);
            for (int i = 0; i < m_numTopologyNodes[currentTopology]; i++)
            {
                for (int j = 0; j < m_Divxi[currentTopology].size(); j++)
                {
                    GxiByDivxi[i] += m_Gxi[currentTopology][j][i] * m_Divxi[currentTopology][j];
                    GxiByDiveta[i] += m_Gxi[currentTopology][j][i] * m_Diveta[currentTopology][j];
                    GetaByDivxi[i] += m_Geta[currentTopology][j][i] * m_Divxi[currentTopology][j];
                    GetaByDiveta[i] += m_Geta[currentTopology][j][i] * m_Diveta[currentTopology][j];
                }
            }

            for (int i = 0; i < m_numTopologyNodes[currentTopology]; i++)
            {
                m_wSmoother[n][i] -= MatrixNorm(a1, a1, DGinvDxi) * m_Jxi[currentTopology][i] +
                    MatrixNorm(a1, a2, DGinvDeta) * m_Jxi[currentTopology][i] +
                    MatrixNorm(a2, a1, DGinvDxi) * m_Jeta[currentTopology][i] +
                    MatrixNorm(a2, a2, DGinvDeta) * m_Jeta[currentTopology][i];
                m_wSmoother[n][i] += MatrixNorm(a1, a1, currentGinv) * GxiByDivxi[i] +
                    MatrixNorm(a1, a2, currentGinv) * GxiByDiveta[i] +
                    MatrixNorm(a2, a1, currentGinv) * GetaByDivxi[i] +
                    MatrixNorm(a2, a2, currentGinv) * GetaByDiveta[i];
            }

            double alpha = 0.0;
            for (int i = 1; i < m_numTopologyNodes[currentTopology]; i++)
            {
                alpha = std::max(alpha, -m_wSmoother[n][i]) / std::max(1.0, m_ww2[currentTopology][i]);
            }

            double sumValues = 0.0;
            for (int i = 1; i < m_numTopologyNodes[currentTopology]; i++)
            {
                m_wSmoother[n][i] = m_wSmoother[n][i] + alpha * std::max(1.0, m_ww2[currentTopology][i]);
                sumValues += m_wSmoother[n][i];
            }
            m_wSmoother[n][0] = -sumValues;
            for (int i = 0; i < m_numTopologyNodes[currentTopology]; i++)
            {
                m_wSmoother[n][i] = -m_wSmoother[n][i] / (-sumValues + 1e-8);
            }
        }

    }
    return true;
}


bool GridGeom::Orthogonalization::ComputeSmootherTopologies(const Mesh& mesh)
{
    bool successful = InitializeSmoother(mesh);

    if (!successful)
    {
        return false;
    }

    for (auto n = 0; n < mesh.GetNumNodes(); n++)
    {
        int numSharedFaces = 0;
        int numConnectedNodes = 0;
        if (successful)
        {
            std::fill(m_sharedFacesCache.begin(), m_sharedFacesCache.end(), -1);
            std::fill(m_connectedNodesCache.begin(), m_connectedNodesCache.end(), 0);
            successful = SmootherNodeAdministration(mesh, n, numSharedFaces, numConnectedNodes);
        }

        if (successful)
        {
            std::fill(m_xiCache.begin(), m_xiCache.end(), 0.0);
            std::fill(m_etaCache.begin(), m_etaCache.end(), 0.0);
            successful = SmootherComputeNodeXiEta(mesh, n, numSharedFaces, numConnectedNodes);
        }

        if (successful)
        {
            successful = SaveSmootherNodeTopologyIfNeeded(n, numSharedFaces, numConnectedNodes);
        }

        if (successful)
        {
            m_maximumNumConnectedNodes = std::max(m_maximumNumConnectedNodes, numConnectedNodes);
            m_maximumNumSharedFaces = std::max(m_maximumNumSharedFaces, numSharedFaces);
        }

    }

    if (!successful)
    {
        return false;
    }

}

bool GridGeom::Orthogonalization::ComputeSmootherOperators(const Mesh& mesh)
{
    // allocate local operators for unique topologies
    m_Az.resize(m_numTopologies);
    m_Gxi.resize(m_numTopologies);
    m_Geta.resize(m_numTopologies);
    m_Divxi.resize(m_numTopologies);
    m_Diveta.resize(m_numTopologies);
    m_Jxi.resize(m_numTopologies);
    m_Jeta.resize(m_numTopologies);
    m_ww2.resize(m_numTopologies);

    // allocate caches
    m_boundaryEdgesCache.resize(2, -1);
    m_leftXFaceCenterCache.resize(maximumNumberOfEdgesPerNode, 0.0);
    m_leftYFaceCenterCache.resize(maximumNumberOfEdgesPerNode, 0.0);
    m_rightXFaceCenterCache.resize(maximumNumberOfEdgesPerNode, 0.0);
    m_rightYFaceCenterCache.resize(maximumNumberOfEdgesPerNode, 0.0);
    m_xisCache.resize(maximumNumberOfEdgesPerNode, 0.0);
    m_etasCache.resize(maximumNumberOfEdgesPerNode, 0.0);

    std::vector<bool> isNewTopology(m_numTopologies, true);
    
    bool successful = true;

    for (auto n = 0; n < mesh.GetNumNodes() ; n++)
    {
        // for each node, the associated topology
        int currentTopology = m_nodeTopologyMapping[n];

        if (isNewTopology[currentTopology])
        {
            isNewTopology[currentTopology] = false;

            // Compute node operators
            if (successful)
            {
                successful = AllocateSmootherNodeOperators(currentTopology);
            }

            if (successful)
            {
                successful = ComputeSmootherOperatorsNode(mesh, n);
            } 
        }
    }

    return successful;
}

bool GridGeom::Orthogonalization::ComputeLocalCoordinates(const Mesh& mesh)
{
    if(mesh.m_projection == Projections::sphericalAccurate)
    {
        //TODO : missing implementation
        return true;
    }

    return true;
}

bool GridGeom::Orthogonalization::ComputeSmootherOperatorsNode(const Mesh& mesh, 
                                                               int currentNode)
{
    // the current topology index
    const int currentTopology = m_nodeTopologyMapping[currentNode];

    for (int f = 0; f < m_numTopologyFaces[currentTopology]; f++)
    {
        if (m_topologySharedFaces[currentTopology][f] < 0 || mesh.m_nodesTypes[currentNode] == 3) 
        {
            continue;
        }

        int edgeLeft = f + 1;
        int edgeRight = edgeLeft + 1; 
        if (edgeRight > m_numTopologyFaces[currentTopology])
        {
            edgeRight -= m_numTopologyFaces[currentTopology];
        }

        const auto xiLeft = m_topologyXi[currentTopology][edgeLeft];
        const auto xiRight = m_topologyXi[currentTopology][edgeRight];
        const auto etaLeft = m_topologyEta[currentTopology][edgeLeft];
        const auto etaRight = m_topologyEta[currentTopology][edgeRight];

        const double edgeLeftSquaredDistance = std::sqrt(xiLeft * xiLeft + etaLeft * etaLeft + 1e-16);
        const double edgeRightSquaredDistance = std::sqrt(xiRight * xiRight + etaRight * etaRight + 1e-16);
        const double cPhi = (xiLeft * xiRight + etaLeft * etaRight) / (edgeLeftSquaredDistance * edgeRightSquaredDistance);
        const auto numFaceNodes = mesh.GetNumFaceEdges(m_topologySharedFaces[currentTopology][f]);

        // the value of xi and eta needs to be estimated at the circumcenters, calculated the contributions of each node 
        if (numFaceNodes == 3)
        {
            // for triangular faces
            int nodeIndex = FindIndex(mesh.m_facesNodes[m_topologySharedFaces[currentTopology][f]], currentNode);
            const auto nodeLeft = NextCircularBackwardIndex(nodeIndex, numFaceNodes);
            const auto nodeRight = NextCircularForwardIndex(nodeIndex, numFaceNodes);

            double alpha = 1.0 / (1.0 - cPhi * cPhi + 1e-8);
            double alphaLeft = 0.5 * (1.0 - edgeLeftSquaredDistance / edgeRightSquaredDistance * cPhi) * alpha;
            double alphaRight = 0.5 * (1.0 - edgeRightSquaredDistance / edgeLeftSquaredDistance * cPhi) * alpha;

            m_Az[currentTopology][f][m_topologyFaceNodeMapping[currentTopology][f][nodeIndex]] = 1.0 - (alphaLeft + alphaRight);
            m_Az[currentTopology][f][m_topologyFaceNodeMapping[currentTopology][f][nodeLeft]]  = alphaLeft;
            m_Az[currentTopology][f][m_topologyFaceNodeMapping[currentTopology][f][nodeRight]] = alphaRight;
        }
        else
        {
            // for non-triangular faces
            for (int i = 0; i < m_topologyFaceNodeMapping[currentTopology][f].size(); i++)
            {
                m_Az[currentTopology][f][m_topologyFaceNodeMapping[currentTopology][f][i]] = 1.0 / double(numFaceNodes);
            }
        }
    }

    // Initialize caches
    std::fill(m_boundaryEdgesCache.begin(), m_boundaryEdgesCache.end(), -1);
    std::fill(m_leftXFaceCenterCache.begin(), m_leftXFaceCenterCache.end(), 0.0);
    std::fill(m_leftYFaceCenterCache.begin(), m_leftYFaceCenterCache.end(), 0.0);
    std::fill(m_rightXFaceCenterCache.begin(), m_rightXFaceCenterCache.end(), 0.0);
    std::fill(m_rightYFaceCenterCache.begin(), m_rightYFaceCenterCache.end(), 0.0);
    std::fill(m_xisCache.begin(), m_xisCache.end(), 0.0);
    std::fill(m_etasCache.begin(), m_etasCache.end(), 0.0);

    int faceRightIndex = 0;
    int faceLeftIndex = 0;
    double xiBoundary = 0.0;
    double etaBoundary = 0.0;

    for (int f = 0; f < m_numTopologyFaces[currentTopology]; f++)
    {
        auto edgeIndex = mesh.m_nodesEdges[currentNode][f];
        int otherNode = mesh.m_edges[edgeIndex].first + mesh.m_edges[edgeIndex].second - currentNode;
        int leftFace = mesh.m_edgesFaces[edgeIndex][0];
        faceLeftIndex = FindIndex(m_topologySharedFaces[currentTopology], leftFace);

        // face not found, this happens when the cell is outside of the polygon
        if (m_topologySharedFaces[currentTopology][faceLeftIndex] != leftFace)
        {
            return false;
        }

        //by construction
        double xiOne = m_topologyXi[currentTopology][f+1];
        double etaOne = m_topologyEta[currentTopology][f+1];

        double leftRightSwap = 1.0;
        double leftXi = 0.0;
        double leftEta = 0.0;
        double rightXi = 0.0;
        double rightEta = 0.0;
        double alpha_x = 0.0;
        if (mesh.m_edgesNumFaces[edgeIndex] == 1)
        {
            // Boundary face
            if (m_boundaryEdgesCache[0] < 0)
            {
                m_boundaryEdgesCache[0] = f;
            }
            else
            {
                m_boundaryEdgesCache[1] = f;
            }

            // Swap left and right if the boundary is at the left 
            if (f != faceLeftIndex) 
            {
                leftRightSwap = -1.0;
            }

            // Compute the face circumcenter
            for (int i = 0; i < m_numTopologyNodes[currentTopology]; i++)
            {
                leftXi += m_topologyXi[currentTopology][i] * m_Az[currentTopology][faceLeftIndex][i];
                leftEta += m_topologyEta[currentTopology][i] * m_Az[currentTopology][faceLeftIndex][i];
                m_leftXFaceCenterCache[f] += mesh.m_nodes[m_topologyConnectedNodes[currentTopology][i]].x * m_Az[currentTopology][faceLeftIndex][i];
                m_leftYFaceCenterCache[f] += mesh.m_nodes[m_topologyConnectedNodes[currentTopology][i]].y * m_Az[currentTopology][faceLeftIndex][i];
            }


            double alpha = leftXi * xiOne + leftEta * etaOne;
            alpha = alpha / (xiOne * xiOne + etaOne * etaOne);

            alpha_x = alpha;
            xiBoundary = alpha * xiOne;
            etaBoundary = alpha * etaOne;

            rightXi = 2.0 * xiBoundary - leftXi;
            rightEta = 2.0 * etaBoundary - leftEta;

            const double xBc = (1.0 - alpha) * mesh.m_nodes[currentNode].x + alpha * mesh.m_nodes[otherNode].x;
            const double yBc = (1.0 - alpha) * mesh.m_nodes[currentNode].y + alpha * mesh.m_nodes[otherNode].y;
            m_leftYFaceCenterCache[f] = 2.0 * xBc - m_leftXFaceCenterCache[f];
            m_rightYFaceCenterCache[f] = 2.0 * yBc - m_leftYFaceCenterCache[f];
        }
        else
        {
            faceLeftIndex = f;
            faceRightIndex = NextCircularBackwardIndex(faceLeftIndex, m_numTopologyFaces[currentTopology]);


            if (faceRightIndex < 0) continue;

            auto faceLeft = m_topologySharedFaces[currentTopology][faceLeftIndex];
            auto faceRight = m_topologySharedFaces[currentTopology][faceRightIndex];

            if ((faceLeft != mesh.m_edgesFaces[edgeIndex][0] && faceLeft != mesh.m_edgesFaces[edgeIndex][1]) ||
                (faceRight != mesh.m_edgesFaces[edgeIndex][0] && faceRight != mesh.m_edgesFaces[edgeIndex][1]))
            {
                return false;
            }

            for (int i = 0; i < m_numTopologyNodes[currentTopology]; i++)
            {
                leftXi += m_topologyXi[currentTopology][i] * m_Az[currentTopology][faceLeftIndex][i];
                leftEta += m_topologyEta[currentTopology][i] * m_Az[currentTopology][faceLeftIndex][i];
                rightXi += m_topologyXi[currentTopology][i] * m_Az[currentTopology][faceRightIndex][i];
                rightEta += m_topologyEta[currentTopology][i] * m_Az[currentTopology][faceRightIndex][i];

                m_leftXFaceCenterCache[f] += mesh.m_nodes[m_topologyConnectedNodes[currentTopology][i]].x * m_Az[currentTopology][faceLeftIndex][i];
                m_leftYFaceCenterCache[f] += mesh.m_nodes[m_topologyConnectedNodes[currentTopology][i]].y * m_Az[currentTopology][faceLeftIndex][i];
                m_rightXFaceCenterCache[f] += mesh.m_nodes[m_topologyConnectedNodes[currentTopology][i]].x * m_Az[currentTopology][faceRightIndex][i];
                m_rightYFaceCenterCache[f] += mesh.m_nodes[m_topologyConnectedNodes[currentTopology][i]].y * m_Az[currentTopology][faceRightIndex][i];
            }
        }

        m_xisCache[f] = 0.5 * (leftXi + rightXi);
        m_etasCache[f] = 0.5 * (leftEta + rightEta);

        double exiLR = rightXi - leftXi;
        double eetaLR = rightEta - leftEta;
        double exi01 = xiOne;
        double eeta01 = etaOne;

        double fac = 1.0 / std::abs(exi01 * eetaLR - eeta01 * exiLR + 1e-16);
        double facxi1 = -eetaLR * fac * leftRightSwap;

        double facxi0 = -facxi1;
        double faceta1 = exiLR * fac * leftRightSwap;
        double faceta0 = -faceta1;
        double facxiR = eeta01 * fac * leftRightSwap;
        double facxiL = -facxiR;
        double facetaR = -exi01 * fac * leftRightSwap;
        double facetaL = -facetaR;

        //boundary link
        if (mesh.m_edgesNumFaces[edgeIndex] == 1)
        {
            facxi1 +=  -facxiL * 2.0 * alpha_x;
            facxi0 +=  -facxiL * 2.0 * (1.0 - alpha_x);
            facxiL +=  facxiL;
            //note that facxiR does not exist
            faceta1 +=  - facetaL * 2.0 * alpha_x;
            faceta0 +=  - facetaL * 2.0 * (1.0 - alpha_x);
            facetaL = 2.0 * facetaL;
        }

        int node1 = f + 1;
        int node0 = 0;
        for (int i = 0; i < m_numTopologyNodes[currentTopology]; i++)
        {
            m_Gxi[currentTopology][f][i] = facxiL * m_Az[currentTopology][faceLeftIndex][i];
            m_Geta[currentTopology][f][i] = facetaL * m_Az[currentTopology][faceLeftIndex][i];
            if (mesh.m_edgesNumFaces[edgeIndex] == 2)
            {
                m_Gxi[currentTopology][f][i] +=  facxiR * m_Az[currentTopology][faceRightIndex][i];
                m_Geta[currentTopology][f][i] += facetaR * m_Az[currentTopology][faceRightIndex][i];
            }
        }

        m_Gxi[currentTopology][f][node1] +=  facxi1;
        m_Geta[currentTopology][f][node1] += faceta1;

        m_Gxi[currentTopology][f][node0] += facxi0;
        m_Geta[currentTopology][f][node0] += faceta0;

        //fill the node-based gradient matrix
        m_Divxi[currentTopology][f] = -eetaLR * leftRightSwap;
        m_Diveta[currentTopology][f] = exiLR * leftRightSwap;

        // boundary link
        if (mesh.m_edgesNumFaces[edgeIndex] == 1)
        {
            m_Divxi[currentTopology][f] = 0.5 * m_Divxi[currentTopology][f] + etaBoundary * leftRightSwap;
            m_Diveta[currentTopology][f] = 0.5 * m_Diveta[currentTopology][f] - xiBoundary * leftRightSwap;
        }
    }

    double volxi = 0.0;
    for (int i = 0; i < mesh.m_nodesNumEdges[currentNode]; i++)
    {
        volxi += 0.5 * (m_Divxi[currentTopology][i] * m_xisCache[i] + m_Diveta[currentTopology][i] * m_etasCache[i]);
    }
    if (volxi == 0.0) 
    {
        volxi = 1.0;
    }

    for (int i = 0; i < mesh.m_nodesNumEdges[currentNode]; i++)
    {
        m_Divxi[currentTopology][i] = m_Divxi[currentTopology][i] / volxi;
        m_Diveta[currentTopology][i] = m_Diveta[currentTopology][i] / volxi;
    }

    //compute the node-to-node gradients
    for (int f = 0; f < m_numTopologyFaces[currentTopology]; f++)
    {
        // internal edge
        if (mesh.m_edgesNumFaces[mesh.m_nodesEdges[currentNode][f]] == 2)
        {
            int rightNode = f - 1;
            if (rightNode < 0)
            {
                rightNode += mesh.m_nodesNumEdges[currentNode];
            }
            for (int i = 0; i < m_numTopologyNodes[currentTopology]; i++)
            {
                m_Jxi[currentTopology][i] += m_Divxi[currentTopology][f] * 0.5 * (m_Az[currentTopology][f][i] + m_Az[currentTopology][rightNode][i]);
                m_Jeta[currentTopology][i] += m_Diveta[currentTopology][f] * 0.5 * (m_Az[currentTopology][f][i] + m_Az[currentTopology][rightNode][i]);
            }
        }
        else
        {
            m_Jxi[currentTopology][0]    += m_Divxi[currentTopology][f] * 0.5;
            m_Jxi[currentTopology][f+1]  += m_Divxi[currentTopology][f] * 0.5;
            m_Jeta[currentTopology][0]   += m_Diveta[currentTopology][f] * 0.5;
            m_Jeta[currentTopology][f+1] += m_Diveta[currentTopology][f] * 0.5;
        }
    }

    //compute the weights in the Laplacian smoother
    std::fill(m_ww2[currentTopology].begin(), m_ww2[currentTopology].end(), 0.0);
    for (int n = 0; n < mesh.m_nodesNumEdges[currentNode]; n++)
    {
        for (int i = 0; i < m_numTopologyNodes[currentTopology]; i++)
        {
            m_ww2[currentTopology][i] += m_Divxi[currentTopology][n] * m_Gxi[currentTopology][n][i] + m_Diveta[currentTopology][n] * m_Geta[currentTopology][n][i];
        }
    }

    return true;
}


bool GridGeom::Orthogonalization::SmootherComputeNodeXiEta(const Mesh& mesh,
    int currentNode,
    const int& numSharedFaces,
    const int& numConnectedNodes)
{
    // the angles for the squared nodes connected to the stencil nodes, first the ones directly connected, then the others
    std::vector<double> thetaSquare(numConnectedNodes, doubleMissingValue);
    // for each shared face, a bollean indicating if it is squared or not
    std::vector<bool> isSquareFace(numSharedFaces, false);

    int numNonStencilQuad = 0;

    //loop over the connected edges
    for (int f = 0; f < numSharedFaces; f++)
    {
        auto edgeIndex = mesh.m_nodesEdges[currentNode][f];
        auto nextNode = m_connectedNodesCache[f + 1]; // the first entry is always the stencil node 
        int faceLeft = mesh.m_edgesFaces[edgeIndex][0];
        int faceRigth = faceLeft;

        if (mesh.m_edgesNumFaces[edgeIndex] == 2) 
        {
            faceRigth = mesh.m_edgesFaces[edgeIndex][1];
        }
        
        //check if it is a rectangular node (not currentNode itself) 
        bool isSquare = true;
        for (int e = 0; e < mesh.m_nodesNumEdges[nextNode]; e++)
        {
            auto edge = mesh.m_nodesEdges[nextNode][e];
            for (int ff = 0; ff < mesh.m_edgesNumFaces[edge]; ff++)
            {
                auto face = mesh.m_edgesFaces[edge][ff];
                if (face != faceLeft && face != faceRigth)
                {
                    isSquare = isSquare && mesh.GetNumFaceEdges(face) == 4;
                }
            }
            if (!isSquare)
            {
                break;
            }
        }

        //Compute optimal angle thetaSquare based on the node type
        int leftFaceIndex = f - 1; 
        if (leftFaceIndex < 0) 
        {
            leftFaceIndex = leftFaceIndex + numSharedFaces;
        } 

        if (isSquare)
        {
            if (mesh.m_nodesTypes[nextNode] == 1 || mesh.m_nodesTypes[nextNode] == 4)
            {
                // Inner node
                numNonStencilQuad = mesh.m_nodesNumEdges[nextNode] - 2;
                thetaSquare[f + 1] = (2.0 - double(numNonStencilQuad) * 0.5) * M_PI;
            }
            if (mesh.m_nodesTypes[nextNode] == 2)
            {
                // boundary node
                numNonStencilQuad = mesh.m_nodesNumEdges[nextNode] - 1 - mesh.m_edgesNumFaces[edgeIndex];
                thetaSquare[f + 1] = (1.0 - double(numNonStencilQuad) * 0.5) * M_PI;
            }
            if (mesh.m_nodesTypes[nextNode] == 3)
            {
                //corner node
                thetaSquare[f + 1] = 0.5 * M_PI;
            }

            if (m_sharedFacesCache[f] > 1)
            {
                if (mesh.GetNumFaceEdges(m_sharedFacesCache[f]) == 4) numNonStencilQuad += 1;
            }
            if (m_sharedFacesCache[leftFaceIndex] > 1)
            {
                if (mesh.GetNumFaceEdges(m_sharedFacesCache[leftFaceIndex]) == 4) numNonStencilQuad += 1;
            }
            if (numNonStencilQuad > 3)
            {
                isSquare = false;
            }
        }

        isSquareFace[f] = isSquareFace[f] || isSquare;
        isSquareFace[leftFaceIndex] = isSquareFace[leftFaceIndex] || isSquare;
    }

    for (int f = 0; f < numSharedFaces; f++)
    {
        // boundary face
        if (m_sharedFacesCache[f] < 0) continue;

        // non boundary face 
        if (mesh.GetNumFaceEdges(m_sharedFacesCache[f]) == 4)
        {
            for (int n = 0; n < mesh.GetNumFaceEdges(m_sharedFacesCache[f]); n++)
            {
                if (m_faceNodeMappingCache[f][n] <= numSharedFaces) continue;
                thetaSquare[m_faceNodeMappingCache[f][n]] = 0.5 * M_PI;
            }
        }
    }

    // Compute internal angle
    int numSquaredTriangles = 0;
    int numTriangles = 0;
    double phiSquaredTriangles = 0.0;
    double phiQuads = 0.0;
    double phiTriangles = 0.0;
    double phiTot = 0.0;
    numNonStencilQuad = 0;
    for (int f = 0; f < numSharedFaces; f++)
    {
        // boundary face
        if (m_sharedFacesCache[f] < 0) continue;

        auto numFaceNodes = mesh.GetNumFaceEdges(m_sharedFacesCache[f]);
        double phi = OptimalEdgeAngle(numFaceNodes);

        if (isSquareFace[f] || numFaceNodes == 4)
        {
            int nextNode = f + 2;
            if (nextNode > numSharedFaces)
            {
                nextNode = nextNode - numSharedFaces;
            }
            bool isBoundaryEdge = mesh.m_edgesNumFaces[mesh.m_nodesEdges[currentNode][f]] == 1;
            phi = OptimalEdgeAngle(numFaceNodes, thetaSquare[f + 1], thetaSquare[nextNode], isBoundaryEdge);
            if (numFaceNodes == 3)
            {
                numSquaredTriangles += 1;
                phiSquaredTriangles += phi;
            }
            else if (numFaceNodes == 4)
            {
                numNonStencilQuad += 1;
                phiQuads += phi;
            }
        }
        else
        {
            numTriangles += 1;
            phiTriangles += phi;
        }
        phiTot += phi;
    }


    double factor = 1.0;
    if (mesh.m_nodesTypes[currentNode] == 2) factor = 0.5;
    if (mesh.m_nodesTypes[currentNode] == 3) factor = 0.25;
    double mu = 1.0;
    double muSquaredTriangles = 1.0;
    double muTriangles = 1.0;
    double minPhi = 15.0 / 180.0 * M_PI;
    if (numTriangles > 0)
    {
        muTriangles = (factor * 2.0 * M_PI - (phiTot - phiTriangles)) / phiTriangles;
        muTriangles = std::max(muTriangles, double(numTriangles) * minPhi / phiTriangles);
    }
    else if (numSquaredTriangles > 0)
    {
        muSquaredTriangles = std::max(factor * 2.0 * M_PI - (phiTot - phiSquaredTriangles), double(numSquaredTriangles) * minPhi) / phiSquaredTriangles;
    }

    if (phiTot > 1e-18)
    {
        mu = factor * 2.0 * M_PI / (phiTot - (1.0 - muTriangles) * phiTriangles - (1.0 - muSquaredTriangles) * phiSquaredTriangles);
    }
    else if (numSharedFaces > 0)
    {
        //TODO: add logger and cirr(xk(k0), yk(k0), ncolhl)
        std::string message{ "fatal error in ComputeXiEta: phiTot=0'" };
        m_nodeXErrors.push_back(mesh.m_nodes[currentNode].x);
        m_nodeXErrors.push_back(mesh.m_nodes[currentNode].y);
        return false;
    }

    double phi0 = 0.0;
    double dPhi0 = 0.0;
    double dPhi = 0.0;
    double dTheta = 0.0;
    for (int f = 0; f < numSharedFaces; f++)
    {
        phi0 = phi0 + 0.5 * dPhi;
        if (m_sharedFacesCache[f] < 0)
        {
            if (mesh.m_nodesTypes[currentNode] == 2)
            {
                dPhi = M_PI;
            }
            else if (mesh.m_nodesTypes[currentNode] == 3)
            {
                dPhi = 1.5 * M_PI;
            }
            else
            {
                //TODO: add logger and cirr(xk(k0), yk(k0), ncolhl)
                std::string message{ "fatal error in ComputeXiEta: inappropriate fictitious boundary cell" };
                m_nodeXErrors.push_back(mesh.m_nodes[currentNode].x);
                m_nodeXErrors.push_back(mesh.m_nodes[currentNode].y);
                return false;
            }
            phi0 = phi0 + 0.5 * dPhi;
            continue;
        }

        int numFaceNodes = mesh.GetNumFaceEdges(m_sharedFacesCache[f]);
        if (numFaceNodes > maximumNumberOfEdgesPerNode)
        {
            //TODO: add logger
            return false;
        }

        dPhi0 = OptimalEdgeAngle(numFaceNodes);
        if (isSquareFace[f])
        {
            int nextNode = f + 2; if (nextNode > numSharedFaces) nextNode = nextNode - numSharedFaces;
            bool isBoundaryEdge = mesh.m_edgesNumFaces[mesh.m_nodesEdges[currentNode][f]] == 1;
            dPhi0 = OptimalEdgeAngle(numFaceNodes, thetaSquare[f + 1], thetaSquare[nextNode], isBoundaryEdge);
            if (numFaceNodes == 3)
            {
                dPhi0 = muSquaredTriangles * dPhi0;
            }
        }
        else if (numFaceNodes == 3)
        {
            dPhi0 = muTriangles * dPhi0;
        }

        dPhi = mu * dPhi0;
        phi0 = phi0 + 0.5 * dPhi;

        // determine the index of the current stencil node
        int nodeIndex = FindIndex(mesh.m_facesNodes[m_sharedFacesCache[f]], currentNode);

        // optimal angle
        dTheta = 2.0 * M_PI / double(numFaceNodes);

        // orientation of the face (necessary for folded cells)
        int previousNode = NextCircularForwardIndex(nodeIndex, numFaceNodes);
        int nextNode = NextCircularBackwardIndex(nodeIndex, numFaceNodes);

        if ((m_faceNodeMappingCache[f][nextNode] - m_faceNodeMappingCache[f][previousNode]) == -1 ||
            (m_faceNodeMappingCache[f][nextNode] - m_faceNodeMappingCache[f][previousNode]) == mesh.m_nodesNumEdges[currentNode])
        {
            dTheta = -dTheta;
        }

        double aspectRatio = (1.0 - std::cos(dTheta)) / std::sin(std::abs(dTheta)) * std::tan(0.5 * dPhi);
        double radius = std::cos(0.5 * dPhi) / (1.0 - cos(dTheta));

        for (int n = 0; n < numFaceNodes; n++)
        {
            double theta = dTheta * (n - nodeIndex);
            double xip = radius - radius * std::cos(theta);
            double ethap = -radius * std::sin(theta);

            m_xiCache[m_faceNodeMappingCache[f][n]] = xip * std::cos(phi0) - aspectRatio * ethap * std::sin(phi0);
            m_etaCache[m_faceNodeMappingCache[f][n]] = xip * std::sin(phi0) + aspectRatio * ethap * std::cos(phi0);
        }
    }

    return true;
}

bool GridGeom::Orthogonalization::SmootherNodeAdministration(const Mesh& mesh, 
                                                                  const int currentNode, 
                                                                  int& numSharedFaces, 
                                                                  int& numConnectedNodes)
{
    if (mesh.m_nodesNumEdges[currentNode] < 2) 
    {
        return true;
    }

    // For the currentNode, find the shared faces
    int newFaceIndex = intMissingValue;
    numSharedFaces = 0;
    for (int e = 0; e < mesh.m_nodesNumEdges[currentNode]; e++)
    {
        auto firstEdge = mesh.m_nodesEdges[currentNode][e];

        int secondEdgeIndex = e + 1;
        if (secondEdgeIndex >= mesh.m_nodesNumEdges[currentNode]) 
        {
            secondEdgeIndex = 0;
        }
        
        auto secondEdge = mesh.m_nodesEdges[currentNode][secondEdgeIndex];
        if (mesh.m_edgesNumFaces[firstEdge] < 1 || mesh.m_edgesNumFaces[secondEdge] < 1) 
        {
            continue;
        }

        // find the face shared by the two edges
        int firstFaceIndex = std::max(std::min(mesh.m_edgesNumFaces[firstEdge], int(2)), int(1)) - 1;
        int secondFaceIndex = std::max(std::min(mesh.m_edgesNumFaces[secondEdge], int(2)), int(1)) - 1;

        if (mesh.m_edgesFaces[firstEdge][0] != newFaceIndex &&
           (mesh.m_edgesFaces[firstEdge][0] == mesh.m_edgesFaces[secondEdge][0] || 
            mesh.m_edgesFaces[firstEdge][0] == mesh.m_edgesFaces[secondEdge][secondFaceIndex]))
        {
            newFaceIndex = mesh.m_edgesFaces[firstEdge][0];
        }
        else if (mesh.m_edgesFaces[firstEdge][firstFaceIndex] != newFaceIndex &&
                (mesh.m_edgesFaces[firstEdge][firstFaceIndex] == mesh.m_edgesFaces[secondEdge][0] || 
                 mesh.m_edgesFaces[firstEdge][firstFaceIndex] == mesh.m_edgesFaces[secondEdge][secondFaceIndex]))
        {
            newFaceIndex = mesh.m_edgesFaces[firstEdge][firstFaceIndex];
        }
        else
        {
            newFaceIndex = intMissingValue;
        }

        //corner face (already found in the first iteration)
        if (mesh.m_nodesNumEdges[currentNode] == 2 && e == 1 && mesh.m_nodesTypes[currentNode] == 3)
        {
            if (m_sharedFacesCache[0] == newFaceIndex) newFaceIndex = intMissingValue;
        }
        m_sharedFacesCache[numSharedFaces] = newFaceIndex;
        numSharedFaces += 1;
    }

    // no shared face found
    if (numSharedFaces < 1) 
    {
        return true;
    }

    int connectedNodesIndex = 0;
    m_connectedNodesCache[connectedNodesIndex] = currentNode;

    // edge connected nodes
    for (int e = 0; e < mesh.m_nodesNumEdges[currentNode]; e++)
    {
        const auto edgeIndex = mesh.m_nodesEdges[currentNode][e];
        const auto node = mesh.m_edges[edgeIndex].first + mesh.m_edges[edgeIndex].second - currentNode;
        connectedNodesIndex++;
        m_connectedNodesCache[connectedNodesIndex] = node;
        if (m_connectedNodes[currentNode].size() < connectedNodesIndex + 1)
        {
            m_connectedNodes[currentNode].resize(connectedNodesIndex);
        }
        m_connectedNodes[currentNode][connectedNodesIndex] = node;
    }

    // for each face store the positions of the its nodes in the connectedNodes (compressed array)
    if (m_faceNodeMappingCache.size() < numSharedFaces)
    {
        m_faceNodeMappingCache.resize(numSharedFaces, std::vector<std::size_t>(maximumNumberOfNodesPerFace, 0));
    }
    for (int f = 0; f < numSharedFaces; f++)
    {
        int faceIndex = m_sharedFacesCache[f];
        if (faceIndex < 0) 
        {
            continue;
        }

        // find the stencil node position  in the current face
        int faceNodeIndex = 0;
        auto numFaceNodes = mesh.GetNumFaceEdges(faceIndex);
        for (int n = 0; n < numFaceNodes; n++)
        {
            if (mesh.m_facesNodes[faceIndex][n] == currentNode)
            {
                faceNodeIndex = n;
                break;
            }
        }

        for (int n = 0; n < numFaceNodes; n++)
        {

            if (faceNodeIndex >= numFaceNodes)
            {
                faceNodeIndex -= numFaceNodes;
            }


            int node = mesh.m_facesNodes[faceIndex][faceNodeIndex];

            bool isNewNode = true;
            for (int n = 0; n < connectedNodesIndex + 1; n++)
            {
                if (node == m_connectedNodesCache[n])
                {
                    isNewNode = false;
                    m_faceNodeMappingCache[f][faceNodeIndex] = n;
                    break;
                }
            }

            if (isNewNode)
            {
                connectedNodesIndex++;
                m_connectedNodesCache[connectedNodesIndex] = node;
                m_faceNodeMappingCache[f][faceNodeIndex] = connectedNodesIndex;
                if (m_connectedNodes[currentNode].size() < connectedNodesIndex + 1)
                {
                    m_connectedNodes[currentNode].resize(connectedNodesIndex);
                }
                m_connectedNodes[currentNode][connectedNodesIndex] = node;
            }

            //update node index
            faceNodeIndex += 1;
        }
    }

    // compute the number of connected nodes
    numConnectedNodes = connectedNodesIndex + 1;

    //update connected nodes (kkc)
    m_numConnectedNodes[currentNode] = numConnectedNodes;

    return true;
}


double GridGeom::Orthogonalization::OptimalEdgeAngle(int numFaceNodes, double theta1, double theta2, bool isBoundaryEdge)
{
    double angle = M_PI * (1 - 2.0 / double(numFaceNodes));

    if (theta1 != -1 && theta2 != -1 && numFaceNodes == 3)
    {
        angle = 0.25 * M_PI;
        if (theta1 + theta2 == M_PI && !isBoundaryEdge)
        {
            angle = 0.5 * M_PI;
        }
    }
    return angle;
}


bool GridGeom::Orthogonalization::AspectRatio(const Mesh& mesh)
{
    std::vector<std::vector<double>> averageEdgesLength(mesh.GetNumEdges(), std::vector<double>(2, doubleMissingValue));
    std::vector<double> averageFlowEdgesLength(mesh.GetNumEdges(), doubleMissingValue);
    std::vector<bool> curvilinearGridIndicator(mesh.GetNumNodes() , true);
    std::vector<double> edgesLength(mesh.GetNumEdges(), 0.0);

    for (auto e = 0; e < mesh.GetNumEdges(); e++)
    {
        auto first = mesh.m_edges[e].first;
        auto second = mesh.m_edges[e].second;

        if (first == second) continue;
        double edgeLength = Distance(mesh.m_nodes[first], mesh.m_nodes[second], mesh.m_projection);
        edgesLength[e] = edgeLength;

        Point leftCenter;
        Point rightCenter;
        if (mesh.m_edgesNumFaces[e] > 0)
        {
            leftCenter = mesh.m_facesCircumcenters[mesh.m_edgesFaces[e][0]];
        }
        else
        {
            leftCenter = mesh.m_nodes[first];
        }

        //find right cell center, if it exists
        if (mesh.m_edgesNumFaces[e] == 2)
        {
            rightCenter = mesh.m_facesCircumcenters[mesh.m_edgesFaces[e][1]];
        }
        else
        {
            //otherwise, make ghost node by imposing boundary condition
            double dinry = InnerProductTwoSegments(mesh.m_nodes[first], mesh.m_nodes[second], mesh.m_nodes[first], leftCenter, mesh.m_projection);
            dinry = dinry / std::max(edgeLength * edgeLength, minimumEdgeLength);

            double x0_bc = (1.0 - dinry) * mesh.m_nodes[first].x + dinry * mesh.m_nodes[second].x;
            double y0_bc = (1.0 - dinry) * mesh.m_nodes[first].y + dinry * mesh.m_nodes[second].y;
            rightCenter.x = 2.0 * x0_bc - leftCenter.x;
            rightCenter.y = 2.0 * y0_bc - leftCenter.y;
        }

        averageFlowEdgesLength[e] = Distance(leftCenter, rightCenter, mesh.m_projection);
    }

    // Compute normal length
    for (int f = 0; f < mesh.GetNumFaces(); f++)
    {
        auto numberOfFaceNodes = mesh.GetNumFaceEdges(f);
        if (numberOfFaceNodes < 3) continue;

        for (int n = 0; n < numberOfFaceNodes; n++)
        {
            if (numberOfFaceNodes != 4) curvilinearGridIndicator[mesh.m_facesNodes[f][n]] = false;
            auto edgeIndex = mesh.m_facesEdges[f][n];

            if (mesh.m_edgesNumFaces[edgeIndex] < 1) continue;

            //get the other links in the right numbering
            //TODO: ask why only 3 are requested, why an average length stored in averageEdgesLength is needed?
            //int kkm1 = n - 1; if (kkm1 < 0) kkm1 = kkm1 + numberOfFaceNodes;
            //int kkp1 = n + 1; if (kkp1 >= numberOfFaceNodes) kkp1 = kkp1 - numberOfFaceNodes;
            //
            //std::size_t klinkm1 = mesh.m_facesEdges[f][kkm1];
            //std::size_t klinkp1 = mesh.m_facesEdges[f][kkp1];
            //

            double edgeLength = edgesLength[edgeIndex];
            if (edgeLength != 0.0)
            {
                m_aspectRatios[edgeIndex] = averageFlowEdgesLength[edgeIndex] / edgeLength;
            }

            //quads
            if (numberOfFaceNodes == 4)
            {
                int kkp2 = n + 2; if (kkp2 >= numberOfFaceNodes) kkp2 = kkp2 - numberOfFaceNodes;
                auto klinkp2 = mesh.m_facesEdges[f][kkp2];
                edgeLength = 0.5 * (edgesLength[edgeIndex] + edgesLength[klinkp2]);
            }

            if (averageEdgesLength[edgeIndex][0] == doubleMissingValue)
            {
                averageEdgesLength[edgeIndex][0] = edgeLength;
            }
            else
            {
                averageEdgesLength[edgeIndex][1] = edgeLength;
            }
        }
    }

    if (curvilinearToOrthogonalRatio == 1.0)
        return true;

    for (auto e = 0; e < mesh.GetNumEdges(); e++)
    {
        auto first = mesh.m_edges[e].first;
        auto second = mesh.m_edges[e].second;

        if (first < 0 || second < 0) continue;
        if (mesh.m_edgesNumFaces[e] < 1) continue;
        // Consider only quads
        if (!curvilinearGridIndicator[first] || !curvilinearGridIndicator[second]) continue;

        if (mesh.m_edgesNumFaces[e] == 1)
        {
            if (averageEdgesLength[e][0] != 0.0 && averageEdgesLength[e][0] != doubleMissingValue)
            {
                m_aspectRatios[e] = averageFlowEdgesLength[e] / averageEdgesLength[e][0];
            }
        }
        else
        {
            if (averageEdgesLength[e][0] != 0.0 && averageEdgesLength[e][1] != 0.0 &&
                averageEdgesLength[e][0] != doubleMissingValue && averageEdgesLength[e][1] != doubleMissingValue)
            {
                m_aspectRatios[e] = curvilinearToOrthogonalRatio * m_aspectRatios[e] +
                    (1.0 - curvilinearToOrthogonalRatio) * averageFlowEdgesLength[e] / (0.5 * (averageEdgesLength[e][0] + averageEdgesLength[e][1]));
            }
        }
    }
    return true;
}

bool GridGeom::Orthogonalization::ComputeWeightsAndRhsOrthogonalizer(const Mesh& mesh)
{
    std::fill(m_rhsOrthogonalizer.begin(), m_rhsOrthogonalizer.end(), std::vector<double>(2, 0.0));
    for (auto n = 0; n < mesh.GetNumNodes() ; n++)
    {
        if (mesh.m_nodesTypes[n] != 1 && mesh.m_nodesTypes[n] != 2)
        {
            continue;
        }

        for (auto nn = 0; nn < mesh.m_nodesNumEdges[n]; nn++)
        {

            const auto edgeIndex = mesh.m_nodesEdges[n][nn];
            double aspectRatio = m_aspectRatios[edgeIndex];
            m_wOrthogonalizer[n][nn] = 0.0;

            if (aspectRatio != doubleMissingValue)
            {
                // internal nodes
                m_wOrthogonalizer[n][nn] = aspectRatio;

                if (mesh.m_edgesNumFaces[edgeIndex] == 1)
                {
                    // boundary nodes
                    m_wOrthogonalizer[n][nn] = 0.5 * aspectRatio;

                    // compute the edge length
                    Point neighbouringNode = mesh.m_nodes[m_nodesNodes[n][nn]];
                    double neighbouringNodeDistance = Distance(neighbouringNode, mesh.m_nodes[n], mesh.m_projection);
                    double aspectRatioByNodeDistance = aspectRatio * neighbouringNodeDistance;

                    auto leftFace = mesh.m_edgesFaces[edgeIndex][0];
                    bool flippedNormal;
                    Point normal;
                    NormalVectorInside(mesh.m_nodes[n], neighbouringNode, mesh.m_facesMassCenters[leftFace], normal, flippedNormal, mesh.m_projection);
                    
                    if(mesh.m_projection==Projections::spherical && mesh.m_projection != Projections::sphericalAccurate)
                    {
                        normal.x = normal.x * std::cos(degrad_hp * 0.5 * (mesh.m_nodes[n].y + neighbouringNode.y));
                    }

                    m_rhsOrthogonalizer[n][0] +=  neighbouringNodeDistance * normal.x * 0.5;
                    m_rhsOrthogonalizer[n][1] +=  neighbouringNodeDistance * normal.y * 0.5;
                }

            }
        }

        // normalize
        double factor = std::accumulate(m_wOrthogonalizer[n].begin(), m_wOrthogonalizer[n].end(), 0.0);
        if (std::abs(factor) > 1e-14)
        {
            factor = 1.0 / factor;
            for (auto& w : m_wOrthogonalizer[n]) w = w * factor;
            m_rhsOrthogonalizer[n][0] = factor * m_rhsOrthogonalizer[n][0];
            m_rhsOrthogonalizer[n][1] = factor * m_rhsOrthogonalizer[n][1];
        }

    }
    return true;
}


double GridGeom::Orthogonalization::MatrixNorm(const std::vector<double>& x, const std::vector<double>& y, const std::vector<double>& matCoefficents)
{
    double norm = (matCoefficents[0] * x[0] + matCoefficents[1] * x[1]) * y[0] + (matCoefficents[2] * x[0] + matCoefficents[3] * x[1]) * y[1];
    return norm;
}


bool GridGeom::Orthogonalization::InitializeSmoother(const Mesh& mesh)
{
    // local matrices caches
    m_numConnectedNodes.resize(mesh.GetNumNodes());
    std::fill(m_numConnectedNodes.begin(), m_numConnectedNodes.end(), 0.0);

    m_connectedNodes.resize(mesh.GetNumNodes());
    std::fill(m_connectedNodes.begin(), m_connectedNodes.end(), std::vector<std::size_t>(maximumNumberOfConnectedNodes));

    m_sharedFacesCache.resize(maximumNumberOfEdgesPerNode, -1);
    std::fill(m_sharedFacesCache.begin(), m_sharedFacesCache.end(), -1);

    m_connectedNodesCache.resize(maximumNumberOfConnectedNodes, 0);
    std::fill(m_connectedNodesCache.begin(), m_connectedNodesCache.end(), 0);

    m_faceNodeMappingCache.resize(maximumNumberOfConnectedNodes);
    std::fill(m_faceNodeMappingCache.begin(), m_faceNodeMappingCache.end(), std::vector<std::size_t>(maximumNumberOfNodesPerFace, 0));

    m_xiCache.resize(maximumNumberOfConnectedNodes, 0.0);
    std::fill(m_xiCache.begin(), m_xiCache.end(), 0.0);

    m_etaCache.resize(maximumNumberOfConnectedNodes, 0.0);
    std::fill(m_etaCache.begin(), m_etaCache.end(), 0.0);

    // topology
    m_numTopologies = 0;

    m_nodeTopologyMapping.resize(mesh.GetNumNodes());
    std::fill(m_nodeTopologyMapping.begin(), m_nodeTopologyMapping.end(), -1);

    m_numTopologyNodes.resize(m_topologyInitialSize);
    std::fill(m_numTopologyNodes.begin(), m_numTopologyNodes.end(),-1);

    m_numTopologyFaces.resize(m_topologyInitialSize);
    std::fill(m_numTopologyFaces.begin(), m_numTopologyFaces.end(),-1);

    m_topologyXi.resize(m_topologyInitialSize);
    std::fill(m_topologyXi.begin(), m_topologyXi.end(), std::vector<double>(maximumNumberOfConnectedNodes, 0));

    m_topologyEta.resize(m_topologyInitialSize);
    std::fill(m_topologyEta.begin(), m_topologyEta.end(), std::vector<double>(maximumNumberOfConnectedNodes, 0));

    m_topologySharedFaces.resize(m_topologyInitialSize);
    std::fill(m_topologySharedFaces.begin(), m_topologySharedFaces.end(), std::vector<int>(maximumNumberOfConnectedNodes, -1));

    m_topologyConnectedNodes.resize(m_topologyInitialSize);
    std::fill(m_topologyConnectedNodes.begin(), m_topologyConnectedNodes.end(), std::vector<std::size_t>(maximumNumberOfConnectedNodes, -1));

    m_topologyFaceNodeMapping.resize(m_topologyInitialSize);
    std::fill(m_topologyFaceNodeMapping.begin(), m_topologyFaceNodeMapping.end(), std::vector<std::vector<std::size_t>>(maximumNumberOfConnectedNodes, std::vector<std::size_t>(maximumNumberOfConnectedNodes, -1)));

    return true;
}


bool GridGeom::Orthogonalization::AllocateSmootherNodeOperators(int topologyIndex)
{
    int numSharedFaces = m_numTopologyFaces[topologyIndex];
    int numConnectedNodes = m_numTopologyNodes[topologyIndex];

    //// will reallocate only if necessary
    m_Az[topologyIndex].resize(numSharedFaces);
    std::fill(m_Az[topologyIndex].begin(), m_Az[topologyIndex].end(), std::vector<double>(numConnectedNodes, 0.0));

    m_Gxi[topologyIndex].resize(numSharedFaces);
    std::fill(m_Gxi[topologyIndex].begin(), m_Gxi[topologyIndex].end(), std::vector<double>(numConnectedNodes, 0.0));

    m_Geta[topologyIndex].resize(numSharedFaces);
    std::fill(m_Geta[topologyIndex].begin(), m_Geta[topologyIndex].end(), std::vector<double>(numConnectedNodes, 0.0));

    m_Divxi[topologyIndex].resize(numSharedFaces);
    std::fill(m_Divxi[topologyIndex].begin(), m_Divxi[topologyIndex].end(), 0.0);

    m_Diveta[topologyIndex].resize(numSharedFaces);
    std::fill(m_Diveta[topologyIndex].begin(), m_Diveta[topologyIndex].end(), 0.0);

    m_Jxi[topologyIndex].resize(numConnectedNodes);
    std::fill(m_Jxi[topologyIndex].begin(), m_Jxi[topologyIndex].end(), 0.0);

    m_Jeta[topologyIndex].resize(numConnectedNodes);
    std::fill(m_Jeta[topologyIndex].begin(), m_Jeta[topologyIndex].end(), 0.0);

    m_ww2[topologyIndex].resize(numConnectedNodes);
    std::fill(m_ww2[topologyIndex].begin(), m_ww2[topologyIndex].end(), 0.0);

    return true;
}


bool GridGeom::Orthogonalization::SaveSmootherNodeTopologyIfNeeded(int currentNode,
                                               int numSharedFaces,
                                               int numConnectedNodes)
{
    bool isNewTopology = true;
    for (int topo = 0; topo < m_numTopologies; topo++)
    {
        if (numSharedFaces != m_numTopologyFaces[topo] || numConnectedNodes != m_numTopologyNodes[topo])
        {
            continue;
        }

        isNewTopology = false;
        for (int n = 1; n < numConnectedNodes; n++)
        {
            double thetaLoc = std::atan2(m_etaCache[n], m_xiCache[n]);
            double thetaTopology = std::atan2(m_topologyEta[topo][n], m_topologyXi[topo][n]);
            if (std::abs(thetaLoc - thetaTopology) > m_thetaTolerance)
            {
                isNewTopology = true;
                break;
            }
        }

        if (!isNewTopology)
        {
            m_nodeTopologyMapping[currentNode] = topo;
            break;
        }
    }

    if (isNewTopology)
    {
        m_numTopologies += 1;

        if (m_numTopologies > m_numTopologyNodes.size())
        {
            m_numTopologyNodes.resize(int(m_numTopologies * 1.5), 0);
            m_numTopologyFaces.resize(int(m_numTopologies * 1.5), 0);
            m_topologyXi.resize(int(m_numTopologies * 1.5), std::vector<double>(maximumNumberOfConnectedNodes, 0));
            m_topologyEta.resize(int(m_numTopologies * 1.5), std::vector<double>(maximumNumberOfConnectedNodes, 0));

            m_topologySharedFaces.resize(int(m_numTopologies * 1.5), std::vector<int>(maximumNumberOfEdgesPerNode, -1));
            m_topologyConnectedNodes.resize(int(m_numTopologies * 1.5), std::vector<std::size_t>(maximumNumberOfConnectedNodes, -1));
            m_topologyFaceNodeMapping.resize(int(m_numTopologies * 1.5), std::vector<std::vector<std::size_t>>(maximumNumberOfConnectedNodes, std::vector<std::size_t>(maximumNumberOfConnectedNodes, -1)));
        }

        int topologyIndex = m_numTopologies - 1;
        m_numTopologyNodes[topologyIndex] = numConnectedNodes;
        m_topologyConnectedNodes[topologyIndex] = m_connectedNodesCache;
        m_numTopologyFaces[topologyIndex] = numSharedFaces;
        m_topologySharedFaces[topologyIndex] = m_sharedFacesCache;
        m_topologyXi[topologyIndex] = m_xiCache;
        m_topologyEta[topologyIndex] = m_etaCache;
        m_topologyFaceNodeMapping[topologyIndex] = m_faceNodeMappingCache;
        m_nodeTopologyMapping[currentNode] = topologyIndex;
    }

    return true;
}

bool GridGeom::Orthogonalization::GetOrthogonality(const Mesh& mesh, double* orthogonality)
{
    for(int e=0; e < mesh.GetNumEdges() ; e++)
    {
        orthogonality[e] = doubleMissingValue;
        int firstVertex = mesh.m_edges[e].first;
        int secondVertex = mesh.m_edges[e].second;

        if (firstVertex!=0 && secondVertex !=0)
        {
            if (e < mesh.GetNumEdges() && mesh.m_edgesNumFaces[e]==2 )
            {
                orthogonality[e] = NormalizedInnerProductTwoSegments(mesh.m_nodes[firstVertex], mesh.m_nodes[secondVertex],
                    mesh.m_facesCircumcenters[mesh.m_edgesFaces[e][0]], mesh.m_facesCircumcenters[mesh.m_edgesFaces[e][1]], mesh.m_projection);
                if (orthogonality[e] != doubleMissingValue)
                {
                    orthogonality[e] = std::abs(orthogonality[e]);

                }
            }
        }
    }
    return true;
}

bool GridGeom::Orthogonalization::GetSmoothness(const Mesh& mesh, double* smoothness)
{
    for (int e = 0; e < mesh.GetNumEdges(); e++)
    {
        smoothness[e] = doubleMissingValue;
        int firstVertex = mesh.m_edges[e].first;
        int secondVertex = mesh.m_edges[e].second;

        if (firstVertex != 0 && secondVertex != 0)
        {
            if (e < mesh.GetNumEdges() && mesh.m_edgesNumFaces[e] == 2)
            {
                int leftFace = mesh.m_edgesFaces[e][0];
                int rightFace = mesh.m_edgesFaces[e][1];
                double leftFaceArea = mesh.m_faceArea[leftFace];
                double rightFaceArea = mesh.m_faceArea[rightFace];

                if (leftFaceArea< minimumCellArea || rightFaceArea< minimumCellArea)
                {
                    smoothness[e] = rightFaceArea / leftFaceArea;
                }
                if (smoothness[e] < 1.0) 
                {
                    smoothness[e] = 1.0 / smoothness[e];
                }
            }
        }
    }
    return true;
}


bool GridGeom::Orthogonalization::ComputeJacobian(int currentNode, const Mesh& mesh, std::vector<double>& J) const
{
    const auto currentTopology = m_nodeTopologyMapping[currentNode];
    const auto numNodes = m_numTopologyNodes[currentTopology];
    if (mesh.m_projection == Projections::cartesian)
    {
        J[0] = 0.0;
        J[1] = 0.0;
        J[2] = 0.0;
        J[3] = 0.0;
        for (int i = 0; i < numNodes; i++)
        {
            J[0] += m_Jxi[currentTopology][i] * mesh.m_nodes[m_topologyConnectedNodes[currentTopology][i]].x;
            J[1] += m_Jxi[currentTopology][i] * mesh.m_nodes[m_topologyConnectedNodes[currentTopology][i]].y;
            J[2] += m_Jeta[currentTopology][i] * mesh.m_nodes[m_topologyConnectedNodes[currentTopology][i]].x;
            J[3] += m_Jeta[currentTopology][i] * mesh.m_nodes[m_topologyConnectedNodes[currentTopology][i]].y;
        }
    }
    if (mesh.m_projection == Projections::spherical || mesh.m_projection == Projections::sphericalAccurate)
    {
        double cosFactor = std::cos(mesh.m_nodes[currentNode].y * degrad_hp);
        J[0] = 0.0;
        J[1] = 0.0;
        J[2] = 0.0;
        J[3] = 0.0;
        for (int i = 0; i < numNodes; i++)
        {
            J[0] += m_Jxi[currentTopology][i] * mesh.m_nodes[m_topologyConnectedNodes[currentTopology][i]].x * cosFactor;
            J[1] += m_Jxi[currentTopology][i] * mesh.m_nodes[m_topologyConnectedNodes[currentTopology][i]].y;
            J[2] += m_Jeta[currentTopology][i] * mesh.m_nodes[m_topologyConnectedNodes[currentTopology][i]].x * cosFactor;
            J[3] += m_Jeta[currentTopology][i] * mesh.m_nodes[m_topologyConnectedNodes[currentTopology][i]].y;
        }
    }
    return true;
}

bool GridGeom::Orthogonalization::UpdateNodeCoordinates(int nodeIndex, const Mesh& mesh)
{
    int numConnectedNodes = m_compressedStartNodeIndex[nodeIndex] - m_compressedEndNodeIndex[nodeIndex];    
    double dx0 = 0.0;
    double dy0 = 0.0;
    double increments[2]{ 0.0, 0.0 };
    for (int nn = 1, cacheIndex = m_compressedEndNodeIndex[nodeIndex]; nn < numConnectedNodes; nn++, cacheIndex++)
    {
        ComputeLocalIncrements(m_compressedWeightX[cacheIndex], m_compressedWeightY[cacheIndex], m_compressedNodesNodes[cacheIndex], nodeIndex, mesh, dx0, dy0, increments);
    }

    if (increments[0] <= 1e-8 || increments[1] <= 1e-8)
    {
        return true;
    }

    int firstCacheIndex = nodeIndex * 2;
    dx0 = (dx0 + m_compressedRhs[firstCacheIndex]) / increments[0];
    dy0 = (dy0 + m_compressedRhs[firstCacheIndex + 1]) / increments[1];
    constexpr double relaxationFactor = 0.75;
    if (mesh.m_projection == Projections::cartesian || mesh.m_projection == Projections::spherical)
    {
        double x0 = mesh.m_nodes[nodeIndex].x + dx0;
        double y0 = mesh.m_nodes[nodeIndex].y + dy0;
        static constexpr double relaxationFactorCoordinates = 1.0 - relaxationFactor;

        m_orthogonalCoordinates[nodeIndex].x = relaxationFactor * x0 + relaxationFactorCoordinates * mesh.m_nodes[nodeIndex].x;
        m_orthogonalCoordinates[nodeIndex].y = relaxationFactor * y0 + relaxationFactorCoordinates * mesh.m_nodes[nodeIndex].y;
    }
    if (mesh.m_projection == Projections::sphericalAccurate)
    {
        Point localPoint{ relaxationFactor * dx0, relaxationFactor * dy0 };

        double exxp[3];
        double eyyp[3];
        double ezzp[3];
        ComputeThreeBaseComponents(mesh.m_nodes[nodeIndex], exxp, eyyp, ezzp);

        //get 3D-coordinates in rotated frame
        Cartesian3DPoint cartesianLocalPoint;
        SphericalToCartesian(localPoint, cartesianLocalPoint);

        //project to fixed frame
        Cartesian3DPoint transformedCartesianLocalPoint;
        transformedCartesianLocalPoint.x = exxp[0] * cartesianLocalPoint.x + eyyp[0] * cartesianLocalPoint.y + ezzp[0] * cartesianLocalPoint.z;
        transformedCartesianLocalPoint.y = exxp[1] * cartesianLocalPoint.x + eyyp[1] * cartesianLocalPoint.y + ezzp[1] * cartesianLocalPoint.z;
        transformedCartesianLocalPoint.z = exxp[2] * cartesianLocalPoint.x + eyyp[2] * cartesianLocalPoint.y + ezzp[2] * cartesianLocalPoint.z;

        //tranform to spherical coordinates
        CartesianToSpherical(transformedCartesianLocalPoint, mesh.m_nodes[nodeIndex].x, m_orthogonalCoordinates[nodeIndex]);
    }
    return true;
}

bool GridGeom::Orthogonalization::ComputeLocalIncrements(double wwx, double wwy, int currentNode, int n, const Mesh& mesh, double& dx0, double& dy0, double* increments)
{

    double wwxTransformed;
    double wwyTransformed;
    if (mesh.m_projection == Projections::cartesian)
    {
        wwxTransformed = wwx;
        wwyTransformed = wwy;
        dx0 = dx0 + wwxTransformed * (mesh.m_nodes[currentNode].x - mesh.m_nodes[n].x);
        dy0 = dy0 + wwyTransformed * (mesh.m_nodes[currentNode].y - mesh.m_nodes[n].y);
    }

    if (mesh.m_projection == Projections::spherical)
    {
        wwxTransformed = wwx * earth_radius * degrad_hp *
            std::cos(0.5 * (mesh.m_nodes[n].y + mesh.m_nodes[currentNode].y) * degrad_hp);
        wwyTransformed = wwy * earth_radius * degrad_hp;

        dx0 = dx0 + wwxTransformed * (mesh.m_nodes[currentNode].x - mesh.m_nodes[n].x);
        dy0 = dy0 + wwyTransformed * (mesh.m_nodes[currentNode].y - mesh.m_nodes[n].y);

    }
    if (mesh.m_projection == Projections::sphericalAccurate)
    {
        wwxTransformed = wwx * earth_radius * degrad_hp;
        wwyTransformed = wwy * earth_radius * degrad_hp;

        dx0 = dx0 + wwxTransformed * m_localCoordinates[m_localCoordinatesIndexes[n] + currentNode - 1].x;
        dy0 = dy0 + wwyTransformed * m_localCoordinates[m_localCoordinatesIndexes[n] + currentNode - 1].y;
    }

    increments[0] += wwxTransformed;
    increments[1] += wwyTransformed;

    return true;
}
