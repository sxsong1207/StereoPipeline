// __BEGIN_LICENSE__
//  Copyright (c) 2009-2013, United States Government as represented by the
//  Administrator of the National Aeronautics and Space Administration. All
//  rights reserved.
//
//  The NGT platform is licensed under the Apache License, Version 2.0 (the
//  "License"); you may not use this file except in compliance with the
//  License. You may obtain a copy of the License at
//  http://www.apache.org/licenses/LICENSE-2.0
//
//  Unless required by applicable law or agreed to in writing, software
//  distributed under the License is distributed on an "AS IS" BASIS,
//  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
//  See the License for the specific language governing permissions and
//  limitations under the License.
// __END_LICENSE__

#include <vw/Math/LevenbergMarquardt.h>
#include <vw/Math/Matrix.h>
#include <vw/Camera/CameraModel.h>
#include <vw/Core/Exception.h>

#include <asp/IsisIO/IsisInterfaceSAR.h>

// ISIS 
#include <Camera.h>
#include <CameraDetectorMap.h>
#include <CameraDistortionMap.h>
#include <CameraFocalPlaneMap.h>
#include <Distance.h>
#include <iTime.h>
#include <Target.h>
#include <SurfacePoint.h>
#include <Latitude.h>
#include <Longitude.h>

#include <boost/smart_ptr/scoped_ptr.hpp>

#include <algorithm>
#include <vector>

using namespace vw;
using namespace asp;
using namespace asp::isis;

// Constructor
IsisInterfaceSAR::IsisInterfaceSAR(std::string const& filename):
  IsisInterface(filename), m_alphacube(*m_cube) {

  // Gutting Isis::Camera
  m_distortmap = m_camera->DistortionMap();
  m_focalmap   = m_camera->FocalPlaneMap();
  m_detectmap  = m_camera->DetectorMap();

  // TODO(oalexan1): All this is fragile. Need to find the right internal ISIS
  // function to use to convert ECEF to lon-lat-height and vice-versa.
  Isis::Distance radii[3];
  m_camera->radii(radii);
  // Average the x and y axes (semi-major) 
  double radius1 = (radii[0].meters() + radii[1].meters()) / 2;
  double radius2 = radii[2].meters(); // the z radius (semi-minor axis)
  std::string target_name = m_camera->target()->name().toStdString();
  m_datum = vw::cartography::Datum("D_" + target_name, target_name,
                                   "Reference Meridian", radius1, radius2, 0);
}

Vector2 IsisInterfaceSAR::point_to_pixel(Vector3 const& point) const {

  // TODO(oalexan1): Find the ISIS function for going from ECEF to llh.
  Vector3 llh = m_datum.cartesian_to_geodetic(point);
  if (llh[0] < 0)
    llh[0] += 360.0;

  // TODO(oalexan1): I would have expected that the third argument
  // should be a radius, rather than height above datum. Need to check
  // with the doc.
  Isis::SurfacePoint surfPt(Isis::Latitude (llh[1], Isis::Angle::Degrees),
                            Isis::Longitude(llh[0], Isis::Angle::Degrees),
                            Isis::Distance (llh[2], Isis::Distance::Meters));
  
  if (m_camera->SetGround(surfPt))
    vw_throw(camera::PixelToRayErr() << "Failed in SetGround().");

  return Vector2(m_camera->Sample() - 1.0, m_camera->Line() - 1.0);
}

// TODO(oalexan1): There should be a simpler way based on rotating
// the look direction, but I could not make that one work.
Vector3 IsisInterfaceSAR::pixel_to_vector(Vector2 const& pix) const {

  // Find the camera center
  Vector3 cam_ctr = camera_center(pix);

  // Find the intersection with the ground. The camera center also
  // sets the image pixel, so the intersection with ground is computed
  // already, need to just fetch it.
  
  Vector3 llh(m_camera->UniversalLongitude(), m_camera->UniversalLatitude(), 0.0);
  Vector3 ground_pt = m_datum.geodetic_to_cartesian(llh);

  // The desired vector is the normalized direction from the camera
  // center to the ground
  Vector3 dir = ground_pt - cam_ctr;
  return dir / norm_2(dir);
}

Vector3 IsisInterfaceSAR::camera_center(Vector2 const& pix) const {

  Vector2 isis_pix = pix + Vector2(1,1);

  if (!m_camera->SetImage(isis_pix[0], isis_pix[1]))
    vw_throw(camera::PixelToRayErr() << "Failed in SetImage().");

  m_camera->instrumentPosition(&m_center[0]);
  m_center *= 1000;
  
  return m_center;
}

Quat IsisInterfaceSAR::camera_pose(Vector2 const& pix) const {
  vw_throw(NoImplErr() << "camera_pose() not implemented for ISIS SAR cameras.");
  return m_pose;
}
