#include "geometrycentral/surface/intrinsic_triangulation.h"

#include "geometrycentral/surface/barycentric_coordinate_helpers.h"
#include "geometrycentral/surface/mesh_graph_algorithms.h"
#include "geometrycentral/surface/trace_geodesic.h"
#include "geometrycentral/utilities/elementary_geometry.h"

#include <iomanip>
#include <queue>


namespace geometrycentral {
namespace surface {

IntrinsicTriangulation::IntrinsicTriangulation(ManifoldSurfaceMesh& mesh_, IntrinsicGeometryInterface& inputGeom_)
    : EdgeLengthGeometry(*mesh_.copy().release()), inputMesh(mesh_), inputGeom(inputGeom_),
      intrinsicMesh(dynamic_cast<ManifoldSurfaceMesh*>(&mesh)) {


  // do this here, rather than in the constructer, since we need to call require() first
  inputGeom.requireEdgeLengths();
  edgeLengths = inputGeom.edgeLengths;

  // Make sure the input mesh is triangular
  if (!mesh.isTriangular()) {
    throw std::runtime_error("intrinsic triangulation requires triangle mesh as input");
  }

  // Initialize vertex locations
  vertexLocations = VertexData<SurfacePoint>(mesh);
  for (size_t iV = 0; iV < mesh.nVertices(); iV++) {
    vertexLocations[iV] = SurfacePoint(inputMesh.vertex(iV));
  }

  // == Register the default callback which maintains marked edges
  auto updateMarkedEdges = [&](Edge oldE, Halfedge newHe1, Halfedge newHe2) {
    if (markedEdges.size() > 0 && markedEdges[oldE]) {
      markedEdges[newHe1.edge()] = true;
      markedEdges[newHe2.edge()] = true;
    }
  };
  edgeSplitCallbackList.push_back(updateMarkedEdges);
}

// ======================================================
// ======== Queries & Accessors
// ======================================================


EdgeData<std::vector<SurfacePoint>> IntrinsicTriangulation::traceEdges() {

  EdgeData<std::vector<SurfacePoint>> tracedEdges(mesh);

  for (Edge e : mesh.edges()) {
    Halfedge he = e.halfedge();
    tracedEdges[e] = traceHalfedge(he, false);
  }

  return tracedEdges;
}

bool IntrinsicTriangulation::isDelaunay() {
  for (Edge e : mesh.edges()) {
    if (!isDelaunay(e)) {
      return false;
    }
  }
  return true;
}

bool IntrinsicTriangulation::isDelaunay(Edge e) {
  if (!isFixed(e) && edgeCotanWeight(e) < -triangleTestEPS) {
    return false;
  }
  return true;
}

double IntrinsicTriangulation::minAngleDegrees() {
  double minAngle = std::numeric_limits<double>::infinity();
  for (Corner c : mesh.corners()) {
    minAngle = std::min(minAngle, cornerAngle(c));
  }
  return minAngle * 180. / M_PI;
}

// ======================================================
// ======== Mutators
// ======================================================


Vertex IntrinsicTriangulation::insertCircumcenter(Face f) {

  // === Circumcenter in barycentric coordinates

  Halfedge he0 = f.halfedge();
  double a = edgeLengths[he0.next().edge()];
  double b = edgeLengths[he0.next().next().edge()];
  double c = edgeLengths[he0.edge()];
  double a2 = a * a;
  double b2 = b * b;
  double c2 = c * c;
  Vector3 circumcenterLoc = {a2 * (b2 + c2 - a2), b2 * (c2 + a2 - b2), c2 * (a2 + b2 - c2)};
  circumcenterLoc = normalizeBarycentric(circumcenterLoc);

  // Trace from the barycenter (have to trace from somewhere)
  Vector3 barycenter = Vector3::constant(1. / 3.);
  Vector3 vecToCircumcenter = circumcenterLoc - barycenter;

  // === Trace the ray to find the location of the new point on the intrinsic meshes

  // Data we need from the intrinsic trace
  TraceOptions options;
  if (markedEdges.size() > 0) {
    options.barrierEdges = &markedEdges;
  }
  TraceGeodesicResult intrinsicTraceResult = traceGeodesic(*this, f, barycenter, vecToCircumcenter, options);
  // intrinsicTracer->snapEndToEdgeIfClose(intrinsicCrumbs); TODO
  // SurfacePoint newPositionOnIntrinsic = intrinsicTraceResult.endPoint.inSomeFace();
  SurfacePoint newPositionOnIntrinsic = intrinsicTraceResult.endPoint;

  // If the circumcenter is blocked by an edge, insert the midpoint of that edge instead
  // (which happens to be just want is needed for Chew's 2nd algo).
  if (newPositionOnIntrinsic.type == SurfacePointType::Edge) {
    newPositionOnIntrinsic.tEdge = 0.5;
  }

  // === Phase 3: Add the new vertex
  return insertVertex(newPositionOnIntrinsic);
}

Vertex IntrinsicTriangulation::insertBarycenter(Face f) {
  SurfacePoint barycenterOnIntrinsic(f, Vector3::constant(1. / 3.));
  return insertVertex(barycenterOnIntrinsic);
}

// ======================================================
// ======== High-Level Mutators
// ======================================================
//


void IntrinsicTriangulation::flipToDelaunay() {

  std::deque<Edge> edgesToCheck;
  EdgeData<bool> inQueue(mesh, true);
  for (Edge e : mesh.edges()) {
    edgesToCheck.push_back(e);
  }

  size_t nFlips = 0;
  while (!edgesToCheck.empty()) {

    // Get the top element from the queue of possibily non-Delaunay edges
    Edge e = edgesToCheck.front();
    edgesToCheck.pop_front();
    inQueue[e] = false;

    bool wasFlipped = flipEdgeIfNotDelaunay(e);

    if (!wasFlipped) continue;

    // Handle the aftermath of a flip
    nFlips++;

    // Add neighbors to queue, as they may need flipping now
    Halfedge he = e.halfedge();
    Halfedge heN = he.next();
    Halfedge heT = he.twin();
    Halfedge heTN = heT.next();
    std::vector<Edge> neighEdges = {heN.edge(), heN.next().edge(), heTN.edge(), heTN.next().edge()};
    for (Edge nE : neighEdges) {
      if (!inQueue[nE]) {
        edgesToCheck.push_back(nE);
        inQueue[nE] = true;
      }
    }
  }

  refreshQuantities();
}

void IntrinsicTriangulation::delaunayRefine(double angleThreshDegrees, double circumradiusThresh,
                                            size_t maxInsertions) {


  // Relationship between angles and circumradius-to-edge
  double angleThreshRad = angleThreshDegrees * M_PI / 180.;
  double circumradiusEdgeRatioThresh = 1.0 / (2.0 * std::sin(angleThreshRad));

  // Build a function to test if a face violates the circumradius ratio condition
  auto needsCircumcenterRefinement = [&](Face f) {
    double c = faceCircumradius(f);
    double l = shortestEdge(f);

    bool needsRefinementLength = c > circumradiusThresh;

    // Explicit check allows us to skip degree one vertices (can't make those angles smaller!)
    bool needsRefinementAngle = false;
    for (Halfedge he : f.adjacentHalfedges()) {

      double baseAngle = cornerAngle(he.corner());
      if (baseAngle < angleThreshRad) {

        // If it's already a degree one vertex, nothing we can do here
        bool isDegreeOneVertex = he.next().next() == he.twin();
        if (isDegreeOneVertex) {
          continue;
        }

        // If it's a fixed corner, can't make it smaller
        if (isFixed(he.edge()) && isFixed(he.prevOrbitFace().edge())) {
          continue;
        }

        needsRefinementAngle = true;
      }
    }

    return needsRefinementAngle || needsRefinementLength;
  };

  // Call the general version
  delaunayRefine(needsCircumcenterRefinement, maxInsertions);
}


void IntrinsicTriangulation::delaunayRefine(const std::function<bool(Face)>& shouldRefine, size_t maxInsertions) {

  // Manages a check at the bottom to avoid infinite-looping when numerical baddness happens
  int recheckCount = 0;
  const int MAX_RECHECK_COUNT = 5;

  // Track statistics
  size_t nFlips = 0;
  size_t nInsertions = 0;

  // Initialize queue of (possibly) non-delaunay edges
  std::deque<Edge> delaunayCheckQueue;
  EdgeData<bool> inDelaunayQueue(mesh, false);
  for (Edge e : mesh.edges()) {
    delaunayCheckQueue.push_back(e);
    inDelaunayQueue[e] = true;
  }


  // Return a weight to use for sorting PQ. Usually sorts by biggest area, but also puts faces on boundary first with
  // weight inf.
  auto areaWeight = [&](Face f) {
    for (Edge e : f.adjacentEdges()) {
      if (isFixed(e)) return std::numeric_limits<double>::infinity();
    }
    return faceArea(f);
  };

  // Initialize queue of (possibly) circumradius-violating faces, processing the largest faces first (good heuristic)
  typedef std::pair<double, Face> AreaFace;
  std::priority_queue<AreaFace, std::vector<AreaFace>, std::less<AreaFace>> circumradiusCheckQueue;
  for (Face f : mesh.faces()) {
    if (shouldRefine(f)) {
      circumradiusCheckQueue.push(std::make_pair(areaWeight(f), f));
    }
  }

  // Register a callback which checks the neighbors of an edge for further processing after a flip. It's useful to use a
  // callback, rather than just checking in the loop, because other internal subroutines might perform flips. In
  // particular, removeInsertedVertex() currently performs flips internally, which might trigger updates.
  auto checkNeighborsAfterFlip = [&](Edge e) {
    // std::cout << "  flipped edge " << e << std::endl;
    nFlips++;

    // Add neighboring faces, which might violate circumradius constraint
    std::vector<Face> neighFaces = {e.halfedge().face(), e.halfedge().twin().face()};
    for (Face nF : neighFaces) {
      if (shouldRefine(nF)) {
        circumradiusCheckQueue.push(std::make_pair(areaWeight(nF), nF));
      }
    }

    // Add neighbors to queue, as they may need flipping now
    Halfedge he = e.halfedge();
    Halfedge heN = he.next();
    Halfedge heT = he.twin();
    Halfedge heTN = heT.next();
    std::vector<Edge> neighEdges = {heN.edge(), heN.next().edge(), heTN.edge(), heTN.next().edge()};
    for (Edge nE : neighEdges) {
      if (!inDelaunayQueue[nE]) {
        delaunayCheckQueue.push_back(nE);
        inDelaunayQueue[nE] = true;
      }
    }
  };
  auto flipCallbackHandle = edgeFlipCallbackList.insert(std::end(edgeFlipCallbackList), checkNeighborsAfterFlip);

  // Register a callback, which will be invoked to delete previously-inserted vertices whenever refinment splits an edge
  auto deleteNearbyVertices = [&](Edge e, Halfedge he1, Halfedge he2) {
    // radius of the diametral ball
    double ballRad = std::max(edgeLengths[he1.edge()], edgeLengths[he2.edge()]);
    Vertex newV = he1.vertex();

    // Find all vertices within range.
    // Most properly, this should probably be a polyhedral geodesic ball search, but that creates a dependence on
    // polyhedral shortest paths which is bad for performance and robustness. Using a Dijsktra ball instead seems to
    // work fine in practice, and I think you could argue that the factor of 2 makes it provably correct, due to the
    // stretch factor of a Delaunay triangulation (recalling that deleting extra interior inserted vertices does not
    // affect correctness).
    //
    // std::unordered_map<Vertex, double> nearbyVerts = vertexGeodesicDistanceWithinRadius(*this, newV, ballRad);
    std::unordered_map<Vertex, double> nearbyVerts = vertexDijkstraDistanceWithinRadius(*this, newV, 2. * ballRad);

    // remove inserted vertices
    for (auto p : nearbyVerts) {
      Vertex v = p.first;
      if (v != newV && !isOnFixedEdge(v) && vertexLocations[v].type != SurfacePointType::Vertex) {
        // std::cout << "  removing inserted vertex " << v << std::endl;
        Face fReplace = removeInsertedVertex(v);

        if (fReplace != Face()) {

          // Add adjacent edges for Delaunay check
          for (Edge nE : fReplace.adjacentEdges()) {
            if (!inDelaunayQueue[nE]) {
              delaunayCheckQueue.push_back(nE);
              inDelaunayQueue[nE] = true;
            }
          }

          // Add face for refine check
          if (shouldRefine(fReplace)) {
            circumradiusCheckQueue.push(std::make_pair(areaWeight(fReplace), fReplace));
          }
        }
      }
    }
  };

  // Add our new callback at the end, so it gets invoked after any user-defined callbacks which ought to get called
  // right after the split before we mess with the mesh.
  auto splitCallbackHandle = edgeSplitCallbackList.insert(std::end(edgeSplitCallbackList), deleteNearbyVertices);

  // === Outer iteration: flip and insert until we have a mesh that satisfies both angle and circumradius goals
  do {

    // == First, flip to delaunay
    while (!delaunayCheckQueue.empty()) {

      // Get the top element from the queue of possibily non-Delaunay edges
      Edge e = delaunayCheckQueue.front();
      delaunayCheckQueue.pop_front();
      if (e.isDead()) continue;
      inDelaunayQueue[e] = false;

      flipEdgeIfNotDelaunay(e);

      // Remember that up we registered a callback up above which checks neighbors for subsequent processing after a
      // flip.
    }

    // == Second, insert one circumcenter

    // If we've already inserted the max number of points, call it a day
    if (maxInsertions != INVALID_IND && nInsertions == maxInsertions) {
      break;
    }

    // Try to insert just one circumcenter
    if (!circumradiusCheckQueue.empty()) {

      // Get the biggest face
      Face f = circumradiusCheckQueue.top().second;
      double A = circumradiusCheckQueue.top().first;
      circumradiusCheckQueue.pop();
      if (f.isDead()) continue;

      // Two things might have changed that would cause us to skip this entry:
      //   -If the area has changed since this face was inserted in to the queue, skip it. Note that we don't need to
      //    re-add it, because it must have been placed in the queue when its area was changed
      //   - This face might have been flipped to no longer violate constraint
      if (A == areaWeight(f) && shouldRefine(f)) {

        // std::cout << "  refining face " << f << std::endl;
        Vertex newVert = insertCircumcenter(f);
        nInsertions++;

        // Mark everything in the 1-ring as possibly non-Delaunay and possibly violating the circumradius constraint
        for (Face nF : newVert.adjacentFaces()) {

          // Check circumradius constraint
          if (shouldRefine(nF)) {
            circumradiusCheckQueue.push(std::make_pair(areaWeight(nF), nF));
          }

          // Check delaunay constraint
          for (Edge nE : nF.adjacentEdges()) {
            if (!inDelaunayQueue[nE]) {
              delaunayCheckQueue.push_back(nE);
              inDelaunayQueue[nE] = true;
            }
          }
        }
      }

      continue;
    }

    // If the circumradius queue is empty, make sure we didn't miss anything (can happen rarely due to numerics)
    // (but don't do this more than a few times, to avoid getting stuck in an infinite loop when numerical ultra-badness
    // happens)
    if (recheckCount < MAX_RECHECK_COUNT) {
      recheckCount++;
      bool anyFound = false;
      if (delaunayCheckQueue.empty() && circumradiusCheckQueue.empty()) {
        for (Face f : mesh.faces()) {
          if (shouldRefine(f)) {
            circumradiusCheckQueue.push(std::make_pair(areaWeight(f), f));
            anyFound = true;
          }
        }
        for (Edge e : mesh.edges()) {
          if (!isDelaunay(e)) {
            delaunayCheckQueue.push_back(e);
            inDelaunayQueue[e] = true;
            anyFound = true;
          }
        }
      }

      if (!anyFound) {
        // makes sure we don't recheck multiple times in a row
        break;
      }
    }

  } while (!delaunayCheckQueue.empty() || !circumradiusCheckQueue.empty() || recheckCount < MAX_RECHECK_COUNT);

  refreshQuantities();

  edgeSplitCallbackList.erase(splitCallbackHandle);
  edgeFlipCallbackList.erase(flipCallbackHandle);
}


void IntrinsicTriangulation::invokeEdgeFlipCallbacks(Edge e) {
  for (auto& fn : edgeFlipCallbackList) {
    fn(e);
  }
}
void IntrinsicTriangulation::invokeFaceInsertionCallbacks(Face f, Vertex v) {
  for (auto& fn : faceInsertionCallbackList) {
    fn(f, v);
  }
}
void IntrinsicTriangulation::invokeEdgeSplitCallbacks(Edge e, Halfedge he1, Halfedge he2) {
  for (auto& fn : edgeSplitCallbackList) {
    fn(e, he1, he2);
  }
}

} // namespace surface
} // namespace geometrycentral