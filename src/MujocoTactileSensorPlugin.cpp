#include "MujocoTactileSensorPlugin.h"

#include <mujoco/mjplugin.h>
#include <mujoco/mujoco.h>

#include <algorithm>
#include <iostream>
#include <sstream>
#include <string>

namespace mujoco::plugin::sensor
{

namespace
{

/** \brief Checks that a plugin config attribute exists. */
bool checkAttr(const std::string & input)
{
  char * end;
  std::string value = input;
  value.erase(std::remove_if(value.begin(), value.end(), isspace), value.end());
  strtod(value.c_str(), &end);
  return end == value.data() + value.size();
}

/** \brief Converts a string into a numeric vector. */
template<typename T>
void readVector(std::vector<T> & output, const std::string & input)
{
  std::stringstream ss(input);
  std::string item;
  char delim = ' ';
  while(getline(ss, item, delim))
  {
    if(!checkAttr(item))
    {
      continue;
    }
    output.push_back(strtod(item.c_str(), nullptr));
  }
}

/** \brief Returns the index of the first value in `a` that `x` is less than or `n` if no such value exists.

    See https://stackoverflow.com/a/39100135
*/
int lowerBound(const mjtNum a[], int n, mjtNum x)
{
  int l = 0;
  int h = n;
  while(l < h)
  {
    int mid = (l + h) / 2;
    if(x <= a[mid])
    {
      h = mid;
    }
    else
    {
      l = mid + 1;
    }
  }
  return l;
}

/** \brief Two dimensional histogram function. */
void histogram2D(const mjtNum x_data[],
                 const mjtNum y_data[],
                 const mjtNum weights[],
                 int n_data,
                 const mjtNum x_edges[],
                 int n_x_edges,
                 const mjtNum y_edges[],
                 int n_y_edges,
                 mjtNum * histogram,
                 int * counts)
{
  for(int i = 0; i < n_data; ++i)
  {
    mjtNum x = x_data[i];
    mjtNum y = y_data[i];
    int x_idx = lowerBound(x_edges, n_x_edges, x);
    if(x_idx == 0 || x_idx == n_x_edges)
    {
      continue;
    }
    int y_idx = lowerBound(y_edges, n_y_edges, y);
    if(y_idx == 0 || y_idx == n_y_edges)
    {
      continue;
    }
    int index = (y_idx - 1) * (n_x_edges - 1) + (x_idx - 1);
    histogram[index] += weights[i];
    if(counts)
    {
      counts[index]++;
    }
  }
}

/** \brief Evenly spaced numbers over a specified interval. */
void linSpace(mjtNum lower, mjtNum upper, int n, mjtNum array[])
{
  mjtNum increment = n > 1 ? (upper - lower) / (n - 1) : 0;
  for(int i = 0; i < n; ++i)
  {
    *array = lower;
    ++array;
    lower += increment;
  }
}

/** \brief Make bin edges. */
void binEdges(mjtNum * x_edges, mjtNum * y_edges, int size[2], mjtNum fov[2])
{
  // Make unit bin edges.
  linSpace(-1, 1, size[0] + 1, x_edges);
  linSpace(-1, 1, size[1] + 1, y_edges);

  // Scale by field-of-view.
  mju_scl(x_edges, x_edges, fov[0] * mjPI / 180, size[0] + 1);
  mju_scl(y_edges, y_edges, fov[1] * mjPI / 180, size[1] + 1);
}

/** \brief Permute 3-vector from 0,1,2 to 2,0,1. */
static void xyz2zxy(mjtNum * x)
{
  mjtNum z = x[2];
  x[2] = x[1];
  x[1] = x[0];
  x[0] = z;
}

/** \brief Transform Cartesian (x,y,z) to spherical (azimuth, elevation, radius).

    Transforming Cartesian <-> spherical:
    - The frame points down the z-axis, so a=e=0 corresponds to (0, 0, -r).
    - azimuth (a) corresponds to positive rotation around -y (towards +x).
    - elevation (e) corresponds to positive rotation around +x (towards +y).
*/
void cartesianToSpherical(const mjtNum xyz[3], mjtNum aer[3])
{
  mjtNum x = xyz[0], y = xyz[1], z = xyz[2];
  aer[0] = mju_atan2(x, -z);
  aer[1] = mju_atan2(y, mju_sqrt(x * x + z * z));
  aer[2] = mju_sqrt(x * x + z * z + y * y);
}

/** \brief Transform spherical (azimuth, elevation, radius) to Cartesian (x,y,z). */
void sphericalToCartesian(const mjtNum aer[3], mjtNum xyz[3])
{
  mjtNum a = aer[0], e = aer[1], r = aer[2];
  xyz[0] = r * mju_cos(e) * mju_sin(a);
  xyz[1] = r * mju_sin(e);
  xyz[2] = -r * mju_cos(e) * mju_cos(a);
}

} // namespace

TactileSensor * TactileSensor::Create(const mjModel * m, mjData * d, int plugin_id)
{
  if(checkAttr(std::string(mj_getPluginConfig(m, plugin_id, "nchannel"))))
  {
    // nchannel
    int nchannel = strtod(mj_getPluginConfig(m, plugin_id, "nchannel"), nullptr);
    if(!nchannel) nchannel = 1;
    if(nchannel < 1 || nchannel > 6)
    {
      mju_error("nchannel must be between 1 and 6");
      return nullptr;
    }

    // size
    std::vector<int> size;
    std::string size_str = std::string(mj_getPluginConfig(m, plugin_id, "size"));
    readVector(size, size_str.c_str());
    if(size.size() != 2)
    {
      mju_error("Both horizontal and vertical resolutions must be specified");
      return nullptr;
    }
    if(size[0] <= 0 || size[1] <= 0)
    {
      mju_error("Horizontal and vertical resolutions must be positive");
      return nullptr;
    }

    // field of view
    std::vector<mjtNum> fov;
    std::string fov_str = std::string(mj_getPluginConfig(m, plugin_id, "fov"));
    readVector(fov, fov_str.c_str());
    if(fov.size() != 2)
    {
      mju_error("Both horizontal and vertical fields of view must be specified");
      return nullptr;
    }
    if(fov[0] <= 0 || fov[0] > 180)
    {
      mju_error("`fov[0]` must be a float between (0, 180] degrees");
      return nullptr;
    }
    if(fov[1] <= 0 || fov[1] > 90)
    {
      mju_error("`fov[1]` must be a float between (0, 90] degrees");
      return nullptr;
    }

    return new TactileSensor(m, d, plugin_id, nchannel, size.data(), fov.data());
  }
  else
  {
    mju_error("Invalid or missing parameters in MujocoTactileSensorPlugin");
    return nullptr;
  }
}

TactileSensor::TactileSensor(const mjModel * m, mjData * d, int plugin_id, int nchannel, int size[2], mjtNum fov[2])
: nchannel_(nchannel), size_{size[0], size[1]}, fov_{fov[0], fov[1]}
{
  // Set sensor_id_
  for(int sensor_id = 0; sensor_id < m->nsensor; ++sensor_id)
  {
    if(m->sensor_type[sensor_id] == mjSENS_PLUGIN && m->sensor_plugin[sensor_id] == plugin_id)
    {
      sensor_id_ = sensor_id;
      break;
    }
  }

  // Make sure sensor is attached to a site.
  if(m->sensor_objtype[sensor_id_] != mjOBJ_SITE)
  {
    mju_error("MujocoTactileSensorPlugin must be attached to a site");
  }

  // Allocate distance array.
  distance_.resize(size[0] * size[1], 0);

  std::cout << "[TactileSensor] Construct." << std::endl;
}

void TactileSensor::reset(const mjModel * m, int plugin_id) {}

void TactileSensor::compute(const mjModel * m, mjData * d, int plugin_id)
{
  mjMARKSTACK;

  // Clear sensordata and distance matrix.
  mjtNum * sensordata = d->sensordata + m->sensor_adr[sensor_id_];
  mju_zero(sensordata, m->sensor_dim[sensor_id_]);
  int frame = size_[0] * size_[1];
  mju_zero(distance_.data(), frame);

  // Get site id.
  int site_id = m->sensor_objid[sensor_id_];

  // Count contacts.
  int ncon = 0;
  int parent_body = m->site_bodyid[site_id];
  int parent_weld = m->body_weldid[parent_body];
  for(int i = 0; i < d->ncon; i++)
  {
    int body1 = m->body_weldid[m->geom_bodyid[d->contact[i].geom1]];
    int body2 = m->body_weldid[m->geom_bodyid[d->contact[i].geom2]];
    if(body1 == parent_weld || body2 == parent_weld)
    {
      ncon++;
    }
  }

  // No contacts, return.
  if(!ncon)
  {
    mjFREESTACK;
    return;
  }

  // Get site frame.
  mjtNum * site_pos = d->site_xpos + 3 * site_id;
  mjtNum * site_mat = d->site_xmat + 9 * site_id;

  // Allocate contact forces and positions
  mjtNum * forces = mj_stackAlloc(d, ncon * 6);
  mjtNum * positions = mj_stackAlloc(d, ncon * 3);

  // Get forces and positions in spherical coordinates.
  int contact = 0;
  for(int i = 0; i < d->ncon; i++)
  {
    int body1 = m->geom_bodyid[d->contact[i].geom1];
    int weld1 = m->body_weldid[body1];
    int body2 = m->geom_bodyid[d->contact[i].geom2];
    int weld2 = m->body_weldid[body2];

    if(weld1 == parent_weld || weld2 == parent_weld)
    {
      // Get contact force/torque, rotate into world frame, then site frame.
      // Note that contact.frame is column major.
      mjtNum tmp_force[6], tmp1[3];
      mj_contactForce(m, d, i, tmp_force);
      mju_rotVecMatT(tmp1, tmp_force, d->contact[i].frame);
      mju_rotVecMatT(forces + 6 * contact, tmp1, site_mat);
      mju_rotVecMatT(tmp1, tmp_force + 3, d->contact[i].frame);
      mju_rotVecMatT(forces + 6 * contact + 3, tmp1, site_mat);

      // Forces point from the smaller to larger body, so flip sign if
      // the parent body has smaller id.
      if(parent_body < mjMAX(body1, body2))
      {
        mju_scl(forces + 6 * contact, forces + 6 * contact, -1, 6);
      }

      // Permute forces from x,y,z to z,x,y (normal, tangent, tangent)
      xyz2zxy(forces + 6 * contact);
      xyz2zxy(forces + 6 * contact + 3);

      // Get position, rotate into contact frame.
      mjtNum tmp2[3];
      mju_sub3(tmp1, d->contact[i].pos, site_pos);
      mju_rotVecMatT(tmp2, tmp1, site_mat);

      // Transform to spherical coordinates, copy into positions array.
      cartesianToSpherical(tmp2, tmp1);
      for(int k = 0; k < 3; k++)
      {
        positions[k * ncon + contact] = tmp1[k];
      }
      contact++;
    }
  }

  // Transpose forces.
  mjtNum * forcesT = mj_stackAlloc(d, ncon * 6);
  mju_transpose(forcesT, forces, ncon, 6);

  // Allocate bin edges.
  mjtNum * x_edges = mj_stackAlloc(d, size_[0] + 1);
  mjtNum * y_edges = mj_stackAlloc(d, size_[1] + 1);

  // Make bin edges.
  binEdges(x_edges, y_edges, size_, fov_);

  // Compute sensor output.
  for(int i = 0; i < nchannel_; i++)
  {
    if(!mju_isZero(forcesT + i * ncon, ncon))
    {
      histogram2D(positions, positions + ncon, forcesT + i * ncon, ncon, x_edges, size_[0] + 1, y_edges, size_[1] + 1,
                  sensordata + i * frame, nullptr);
    }
  }

  // Allocate count matrix.
  int * counts = mj_stackAllocInt(d, frame);
  for(int i = 0; i < frame; i++) counts[i] = 0;

  // Compute distance matrix (unnormalized).
  histogram2D(positions, positions + ncon, positions + 2 * ncon, ncon, x_edges, size_[0] + 1, y_edges, size_[1] + 1,
              distance_.data(), counts);

  // Normalize distances
  for(int i = 0; i < frame; i++)
  {
    if(counts[i])
    {
      distance_.data()[i] /= counts[i];
    }
  }

  mjFREESTACK;
}

// Thickness of taxel-visualization boxes relative to contact distance.
static const mjtNum kRelativeThickness = 0.02;

void TactileSensor::visualize(const mjModel * m, mjData * d, const mjvOption * opt, mjvScene * scn, int plugin_id)
{
  mjMARKSTACK;

  // Get sensor data.
  mjtNum * sensordata = d->sensordata + m->sensor_adr[sensor_id_];

  // Get maximum absolute normal force.
  mjtNum maxval = 0;
  int frame = size_[0] * size_[1];
  for(int j = 0; j < frame; j++)
  {
    maxval = mju_max(maxval, mju_abs(sensordata[j]));
  }

  // If no normal force readings, quick return.
  if(!maxval)
  {
    mjFREESTACK;
    return;
  }

  // Get site id and frame.
  int site_id = m->sensor_objid[sensor_id_];
  mjtNum * site_pos = d->site_xpos + 3 * site_id;
  mjtNum * site_mat = d->site_xmat + 9 * site_id;
  mjtNum site_quat[4];
  mju_mat2Quat(site_quat, site_mat);

  // Allocate bin edges.
  mjtNum * x_edges = mj_stackAlloc(d, size_[0] + 1);
  mjtNum * y_edges = mj_stackAlloc(d, size_[1] + 1);

  // Make bin edges.
  binEdges(x_edges, y_edges, size_, fov_);

  // Draw geoms.
  for(int i = 0; i < size_[0]; i++)
  {
    for(int j = 0; j < size_[1]; j++)
    {
      mjtNum dist = distance_.data()[j * size_[0] + i];
      if(!dist)
      {
        continue;
      }
      if(scn->ngeom >= scn->maxgeom)
      {
        mj_warning(d, mjWARN_VGEOMFULL, scn->maxgeom);
        mjFREESTACK;
        return;
      }
      else
      {
        // size
        mjtNum size[3] = {dist * 0.5 * (x_edges[i + 1] - x_edges[i]), dist * 0.5 * (y_edges[j + 1] - y_edges[j]),
                          dist * kRelativeThickness};

        // position
        mjtNum pos[3];
        mjtNum aer[3] = {0.5 * (x_edges[i + 1] + x_edges[i]), 0.5 * (y_edges[j + 1] + y_edges[j]),
                         dist * (1 - kRelativeThickness)};
        sphericalToCartesian(aer, pos);
        mju_rotVecMat(pos, pos, site_mat);
        mju_addTo3(pos, site_pos);

        // orientation
        mjtNum a_quat[4];
        mjtNum site_y[3] = {-site_mat[1], -site_mat[4], -site_mat[7]};
        mju_axisAngle2Quat(a_quat, site_y, aer[0]);
        mjtNum e_quat[4];
        mjtNum site_x[3] = {site_mat[0], site_mat[3], site_mat[6]};
        mju_axisAngle2Quat(e_quat, site_x, aer[1]);
        mjtNum quat[4];
        mju_mulQuat(quat, e_quat, site_quat);
        mju_mulQuat(quat, a_quat, quat);
        mjtNum mat[9];
        mju_quat2Mat(mat, quat);

        // color
        float rgba[4] = {1, 1, 1, 1.0};
        for(int k = 0; k < mjMIN(nchannel_, 3); k++)
        {
          rgba[k] = mju_abs(sensordata[k * frame + j * size_[0] + i]) / maxval;
        }

        // draw box geom
        mjvGeom * thisgeom = scn->geoms + scn->ngeom;
        mjv_initGeom(thisgeom, mjGEOM_BOX, size, pos, mat, rgba);
        thisgeom->objtype = mjOBJ_UNKNOWN;
        thisgeom->objid = -1;
        thisgeom->category = mjCAT_DECOR;
        thisgeom->segid = scn->ngeom;
        scn->ngeom++;
      }
    }
  }

  mjFREESTACK;
}

void TactileSensor::RegisterPlugin()
{
  mjpPlugin plugin;
  mjp_defaultPlugin(&plugin);

  plugin.name = "MujocoTactileSensorPlugin";
  plugin.capabilityflags |= mjPLUGIN_SENSOR;

  // Parameterized by 4 attributes.
  const char * attributes[] = {"nchannel", "size", "fov"};
  plugin.nattribute = sizeof(attributes) / sizeof(attributes[0]);
  plugin.attributes = attributes;

  // Stateless.
  plugin.nstate = +[](const mjModel * m, int instance) { return 0; };

  // Sensor dimension = nchannel * size[0] * size[1]
  plugin.nsensordata = +[](const mjModel * m, int instance, int sensor_id)
  {
    int nchannel = strtod(mj_getPluginConfig(m, instance, "nchannel"), nullptr);
    if(!nchannel) nchannel = 1;
    std::vector<int> size;
    std::string size_str = std::string(mj_getPluginConfig(m, instance, "size"));
    readVector(size, size_str.c_str());
    return nchannel * size[0] * size[1];
  };

  // Can only run after forces have been computed.
  plugin.needstage = mjSTAGE_ACC;

  // Initialization callback.
  plugin.init = +[](const mjModel * m, mjData * d, int instance)
  {
    auto * TactileSensor = TactileSensor::Create(m, d, instance);
    if(!TactileSensor)
    {
      return -1;
    }
    d->plugin_data[instance] = reinterpret_cast<uintptr_t>(TactileSensor);
    return 0;
  };

  // Destruction callback.
  plugin.destroy = +[](mjData * d, int instance)
  {
    delete reinterpret_cast<TactileSensor *>(d->plugin_data[instance]);
    d->plugin_data[instance] = 0;
  };

  // Reset callback.
  plugin.reset = +[](const mjModel * m, double * plugin_state, void * plugin_data, int instance)
  {
    auto * TactileSensor = reinterpret_cast<class TactileSensor *>(plugin_data);
    TactileSensor->reset(m, instance);
  };

  // Compute callback.
  plugin.compute = +[](const mjModel * m, mjData * d, int instance, int capability_bit)
  {
    auto * TactileSensor = reinterpret_cast<class TactileSensor *>(d->plugin_data[instance]);
    TactileSensor->compute(m, d, instance);
  };

  // Visualization callback.
  plugin.visualize = +[](const mjModel * m, mjData * d, const mjvOption * opt, mjvScene * scn, int instance)
  {
    auto * TactileSensor = reinterpret_cast<class TactileSensor *>(d->plugin_data[instance]);
    TactileSensor->visualize(m, d, opt, scn, instance);
  };

  // Register the plugin.
  mjp_registerPlugin(&plugin);
}

mjPLUGIN_LIB_INIT
{
  TactileSensor::RegisterPlugin();
}
} // namespace mujoco::plugin::sensor