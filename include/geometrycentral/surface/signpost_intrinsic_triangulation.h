#pragma once

#include "geometrycentral/surface/embedded_geometry_interface.h"
#include "geometrycentral/surface/intrinsic_geometry_interface.h"
#include "geometrycentral/surface/intrinsic_triangulation.h"
#include "geometrycentral/surface/manifold_surface_mesh.h"
#include "geometrycentral/surface/surface_point.h"
#include "geometrycentral/utilities/elementary_geometry.h"


// An implmementation of the Signpost datastructure from
//   > "Navigating Intrinsic Triangulations". Sharp, Soliman, and Crane. SIGGRAPH 2019


namespace geometrycentral {
namespace surface {


class SignpostIntrinsicTriangulation : public IntrinsicTriangulation {

public:
  // Construct an intrinsic triangulation which sits atop this input mesh. Initially, the input triangulation will
  // just be a copy of the input mesh.
  SignpostIntrinsicTriangulation(ManifoldSurfaceMesh& mesh, IntrinsicGeometryInterface& inputGeom);

  // ======================================================
  // ======== Core Members
  // ======================================================

  // The geometry of the signpost intrinsic triangulation
  HalfedgeData<double> intrinsicHalfedgeDirections; // direction of each halfedge, in radians from [0, angleSum]
  VertexData<double> intrinsicVertexAngleSums;      // vertex cone angle sum
  EdgeData<bool> edgeIsOriginal; // did this edge come from the original triangulation? used mainly for optimizations.

  // ======================================================
  // ======== Queries & Accessors
  // ======================================================

  // Given a point on the input triangulation, returns the corresponding point on the intrinsic triangulation
  SurfacePoint equivalentPointOnIntrinsic(const SurfacePoint& pointOnInput) override;

  // Given a point on the intrinsic triangulation, returns the corresponding point on the input triangulation
  SurfacePoint equivalentPointOnInput(const SurfacePoint& pointOnIntrinsic) override;

  std::vector<SurfacePoint> traceHalfedge(Halfedge he,
                                          bool trimEnd = true) override; // trace a single intrinsic halfedge


  // ======================================================
  // ======== Low-Level Mutators
  // ======================================================

  // If the edge is not Delaunay, flip it. Returns true if flipped.
  bool flipEdgeIfNotDelaunay(Edge e) override;

  // If the edge can be flipped, flip it (must be combinatorially flippable and inside a convex quad). Returns true if
  // flipped.
  bool flipEdgeIfPossible(Edge e, double possibleEPS = 1e-6) override;

  // Flip an edge, where the caller specifies geometric data for the updated edge, rather than it being computed. Must
  // be flippable. Experts only.
  void flipEdgeManual(Edge e, double newLength, double forwardAngle, double reverseAngle, bool isOrig,
                      bool reverseFlip = false) override;

  // Insert a new vertex in to the intrinsic triangulation
  Vertex insertVertex(SurfacePoint newPositionOnIntrinsic) override;

  // Insert the circumcenter of a face in to the triangulation. Returns the newly created intrinsic vertex.
  Vertex insertCircumcenter(Face f) override;

  // Insert the barycenter of a face in to the triangulation. Returns the newly created intrinsic vertex.
  Vertex insertBarycenter(Face f) override;

  // Remove an (inserted) vertex from the triangulation.
  // Note: if something goes terribly (numerically?) wrong, will exit without removing the vertex.
  Face removeInsertedVertex(Vertex v) override;

  // Split a halfedge
  Halfedge splitEdge(Halfedge he, double tSplit) override;

protected:
  
  // mutation helpers
  void flipEdgeInternal(Edge e, double newLen) override;

private:
  // ======================================================
  // ======== Geometry Interface
  // ======================================================
  //
  // Satisfy the requirements of the IntrinsicGeometryInterface

  // Override the compute edge lengths method from intrinsic geometry.
  virtual void computeEdgeLengths() override;

  // Override the halfedge vectors method from intrinsic geometry
  virtual void computeHalfedgeVectorsInVertex() override;


  // ======================================================
  // ======== Helpers
  // ======================================================

  // Insertion helpers
  Vertex insertVertex_face(SurfacePoint newPositionOnIntrinsic);
  Halfedge insertVertex_edge(SurfacePoint newPositionOnIntrinsic);
  void resolveNewVertex(Vertex newV, SurfacePoint intrinsicPoint);

  // Update a signpost angle from the (counter-)clockwise neighboring angle
  void updateAngleFromCWNeighor(Halfedge he);

  // Map angle to range [0, angleSum)
  double standardizeAngle(Vertex vert, double angle) const;

  // Get vector in rescaled vertex coordinates
  Vector2 halfedgeVector(Halfedge he) const;

  // Scale factor to take Euclidean data to cone data
  double vertexAngleScaling(Vertex v) const;

  // Repopulate the member halfedgeVectorInFace
  void updateFaceBasis(Face f);

  // Is this a marked or boundary edge?
  bool isFixed(Edge e);
  bool isOnFixedEdge(Vertex v); // boundary vertex or on fixed edge

};


} // namespace surface
} // namespace geometrycentral


#include "geometrycentral/surface/signpost_intrinsic_triangulation.ipp"
