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

/// \file image_mosaic.cc
///
/// Tool for creating mosaics of images on disk.
/// - Currently supports one line of images.

#include <limits>

#include <vw/FileIO/DiskImageUtils.h>
#include <vw/Image/Algorithms2.h>
#include <vw/Image/AlgorithmFunctions.h>

#include <asp/Core/Common.h>
#include <asp/Core/Macros.h>
#include <asp/Core/InterestPointMatching.h>

using namespace vw;
namespace po = boost::program_options;


//TODO: REMOVE!
  template<class T>
  std::string num2str(T num){
    std::ostringstream S;
    S << num;
    return S.str();
  }

  
/// GDAL block write sizes must be a multiple to 16 so if the input value is
///  not a multiple of 16 increase it until it is.
int fix_tile_multiple(int &size) {
  const int TILE_MULTIPLE = 16;
  if (size % TILE_MULTIPLE != 0)
    size = ((size / TILE_MULTIPLE) + 1) * TILE_MULTIPLE;
}
  
  
// TODO: How do the other versions work??
// TODO: There are now three versions of this that need to be consolidated!!!
template<class ImageT>
void centerline_weights3(ImageT const& img, ImageView<double> & weights,
                         double hole_fill_value=0, double border_fill_value=-1, 
                         BBox2i roi=BBox2i()){

  int numRows = img.rows();
  int numCols = img.cols();

  // Arrays to be returned out of this function
  std::vector<double> hCenterLine  (numRows, 0);
  std::vector<double> hMaxDistArray(numRows, 0);
  std::vector<double> vCenterLine  (numCols, 0);
  std::vector<double> vMaxDistArray(numCols, 0);

  std::vector<int> minValInRow(numRows, 0);
  std::vector<int> maxValInRow(numRows, 0);
  std::vector<int> minValInCol(numCols, 0);
  std::vector<int> maxValInCol(numCols, 0);

  for (int k = 0; k < numRows; k++){
    minValInRow[k] = numCols;
    maxValInRow[k] = 0;
  }
  for (int col = 0; col < numCols; col++){
    minValInCol[col] = numRows;
    maxValInCol[col] = 0;
  }

  // Note that we do just a single pass through the image to compute
  // both the horizontal and vertical min/max values.
  for (int row = 0 ; row < numRows; row++) {
    for (int col = 0; col < numCols; col++) {

      if ( !is_valid(img(col,row)) ) continue;
      
      // Record the first and last valid column in each row
      if (col < minValInRow[row]) minValInRow[row] = col;
      if (col > maxValInRow[row]) maxValInRow[row] = col;
      
      // Record the first and last valid row in each column
      if (row < minValInCol[col]) minValInCol[col] = row;
      if (row > maxValInCol[col]) maxValInCol[col] = row;   
    }
  }
  
  // For each row, record central column and the column width
  for (int row = 0; row < numRows; row++) {
    hCenterLine   [row] = (minValInRow[row] + maxValInRow[row])/2.0;
    hMaxDistArray [row] =  maxValInRow[row] - minValInRow[row];
    if (hMaxDistArray[row] < 0){
      hMaxDistArray[row]=0;
    }
  }

  // For each row, record central column and the column width
  for (int col = 0 ; col < numCols; col++) {
    vCenterLine   [col] = (minValInCol[col] + maxValInCol[col])/2.0;
    vMaxDistArray [col] =  maxValInCol[col] - minValInCol[col];
    if (vMaxDistArray[col] < 0){
      vMaxDistArray[col]=0;
    }
  }

  BBox2i output_bbox = roi;
  if (roi.empty())
    output_bbox = bounding_box(img);

  // Compute the weighting for each pixel in the image
  weights.set_size(output_bbox.width(), output_bbox.height());
  fill(weights, 0);
  
  for (int row = output_bbox.min().y(); row < output_bbox.max().y(); row++){
    for (int col = output_bbox.min().x(); col < output_bbox.max().x(); col++){
      bool inner_row = ((row >= minValInCol[col]) && (row <= maxValInCol[col]));
      bool inner_col = ((col >= minValInRow[row]) && (col <= maxValInRow[row]));
      bool inner_pixel = inner_row && inner_col;
      Vector2 pix(col, row);
      double new_weight = 0; // Invalid pixels usually get zero weight
      if (is_valid(img(col,row))) {
        double weight_h = compute_line_weights(pix, true,  hCenterLine, hMaxDistArray);
        double weight_v = compute_line_weights(pix, false, vCenterLine, vMaxDistArray);
        new_weight = std::min(weight_h, weight_v);
      }
      else { // Invalid pixel
        if (inner_pixel)
          new_weight = hole_fill_value;
        else // Border pixel
          new_weight = border_fill_value;
      }
      weights(col-output_bbox.min().x(), row-output_bbox.min().y()) = new_weight;
      
    }
  }

} // End function weights_from_centerline



struct Options : vw::cartography::GdalWriteOptions {
  std::vector<std::string> image_files;
  std::string orientation, output_image, output_type;
  int    overlap_width, band, blend_radius;
  bool   has_input_nodata_value, has_output_nodata_value;
  double input_nodata_value, output_nodata_value;
  Options(): has_input_nodata_value(false), has_output_nodata_value(false),
             input_nodata_value (std::numeric_limits<double>::quiet_NaN()),
             output_nodata_value(std::numeric_limits<double>::quiet_NaN()){}
};

/// Load an input image, respecting the user parameters.
void get_input_image(std::string const& path,
                     Options const& opt,
                     ImageViewRef<float> &image,
                     double &nodata) {
  // Extract the desired band
  int num_bands = get_num_channels(path);
  if (num_bands == 1){
    image = DiskImageView<float>(path);
  }else{
    // Multi-band image. Pick the desired band.
    int channel = opt.band - 1;  // In VW, bands start from 0, not 1.
    image = select_channel(read_channels<1, float>(path, channel), 0);
  }

  // Read nodata-value from disk.
  DiskImageResourceGDAL in_rsrc(path);
  bool has_nodata = in_rsrc.has_nodata_read();
  if (has_nodata)
    nodata = in_rsrc.nodata_read();
  else
    nodata = std::numeric_limits<double>::quiet_NaN();
}

/// Get a list of matched IP, looking in certain image regions.
void match_ip_in_regions(std::string const& image_file1,
                         std::string const& image_file2,
                         BBox2i const& roi1,
                         BBox2i const& roi2,
                         std::vector<ip::InterestPoint> &matched_ip1,
                         std::vector<ip::InterestPoint> &matched_ip2,
                         Options const& opt) {

  // Load the input images
  ImageViewRef<float> image1,  image2;
  double              nodata1, nodata2;
  get_input_image(image_file1, opt, image1, nodata1);
  get_input_image(image_file2, opt, image2, nodata2);

  // Now find and match interest points in the selected regions
  int ip_per_tile = 0; // Let this be computed automatically.
  asp::detect_match_ip(matched_ip1, matched_ip2,
                       crop(image1, roi1),
                       crop(image2, roi2), ip_per_tile,
                       "", "", nodata1, nodata2); // TODO: Cache the IP?

  // TODO: This should be a function!
  // Adjust the IP to account for the search ROIs.
  for (size_t i=0; i<matched_ip1.size(); ++i) {
    matched_ip1[i].x  += roi1.min()[0];
    matched_ip1[i].ix += roi1.min()[0];
    matched_ip1[i].y  += roi1.min()[1];
    matched_ip1[i].iy += roi1.min()[1];
    
    matched_ip2[i].x  += roi2.min()[0];
    matched_ip2[i].ix += roi2.min()[0];
    matched_ip2[i].y  += roi2.min()[1];
    matched_ip2[i].iy += roi2.min()[1];
  }
  std::cout << "matched_ip1.size() = " << matched_ip1.size() << std::endl;
} // End function match_ip_in_regions


/// Compute an affine transform between images, searching for IP in
///  the specified regions.
Matrix<double> affine_ip_matching(std::string const& image_file1,
                                  std::string const& image_file2,
                                  BBox2i const& roi1,
                                  BBox2i const& roi2,
                                  Options const& opt) {

  // Find IP, looking in only the specified regions.
  std::vector<ip::InterestPoint> matched_ip1, matched_ip2;
  match_ip_in_regions(image_file1, image_file2, roi1, roi2,
                      matched_ip1,  matched_ip2, opt);

  // Clean up lists.
  std::vector<Vector3> ransac_ip1 = iplist_to_vectorlist(matched_ip1);
  std::vector<Vector3> ransac_ip2 = iplist_to_vectorlist(matched_ip2);

  // TODO: Experiment with these!
  // RANSAC parameters.
  const int    num_iterations   = 100;
  const double inlier_threshold = 10;
  const int    min_num_output_inliers = ransac_ip1.size()/2;
  const bool   reduce_min_num_output_inliers_if_no_fit = true;

  std::cout << "min_num_output_inliers = " << min_num_output_inliers << std::endl;
  
  Matrix<double> tf;
  std::vector<size_t> indices;
  try {
    vw::math::RandomSampleConsensus<vw::math::AffineFittingFunctor,
                                    vw::math::InterestPointErrorMetric>
                                      ransac(vw::math::AffineFittingFunctor(),
                                             vw::math::InterestPointErrorMetric(),
                                             num_iterations,
                                             inlier_threshold,
                                             min_num_output_inliers,
                                             reduce_min_num_output_inliers_if_no_fit);
    tf = ransac( ransac_ip2, ransac_ip1 );
    indices = ransac.inlier_indices(tf, ransac_ip2, ransac_ip1 );
  } catch (...) {
    vw_throw( ArgumentErr() << "Automatic Alignment failed in RANSAC fit!");
  }
/*
  // Keeping only inliers
  std::vector<ip::InterestPoint> inlier_ip1, inlier_ip2;
  for ( size_t i = 0; i < indices.size(); i++ ) {
    inlier_ip1.push_back( matched_ip1[indices[i]] );
    inlier_ip2.push_back( matched_ip2[indices[i]] );
  }
  matched_ip1 = inlier_ip1;
  matched_ip2 = inlier_ip2;
*/

  return tf;
}

// TODO: Pass in image ref instead of paths?
/// Compute the transform from image1 to image2
///  (the top left corner of image1 is (0,0))
Matrix<double> compute_relative_transform(std::string const& image1,
                                          std::string const& image2,
                                          Options const& opt) {

  Vector2i size1 = file_image_size(image1);
  Vector2i size2 = file_image_size(image2);

  // Set up the ROIs for the two images based on the selected orientation.
  // - Currently only horizontal orientation is supported.
  BBox2i roi1, roi2;
  if (opt.orientation == "horizontal") {
    roi1.min() = Vector2(size1[0]-opt.overlap_width, 0);
    roi1.max() = size1; // Bottom right corner
    roi2.min() = Vector2(0,0); // Top left corner
    roi2.max() = Vector2(opt.overlap_width, size2[1]); // Bottom right corner
  }

  std::cout << "roi1 = " << roi1 << std::endl;
  std::cout << "roi2 = " << roi2 << std::endl;
  
  if (roi1.empty() || roi2.empty())
    vw_throw( ArgumentErr() << "Unrecognized image orientation!");

  Matrix<double> tf = affine_ip_matching(image1, image2, roi1, roi2, opt);
  return tf;

} // End function compute_relative_transform


/// Compute the positions of each image relative to the first image.
/// - The top left corner of the first image is coordinate 0,0 in the output image.
void compute_all_image_positions(Options const& opt,
                                 std::vector<boost::shared_ptr<vw::Transform> > & transforms,
                                 std::vector<BBox2i>          & bboxes,
                                 Vector2i                     & output_image_size) {

  const size_t num_images = opt.image_files.size();

  transforms.resize(num_images);
  bboxes.resize(num_images);

  // Init the bounding box to contain the first image
  BBox2i output_bbox;
  Vector2i image_size = file_image_size(opt.image_files[0]);
  output_bbox.grow(Vector2i(0,0));
  output_bbox.grow(image_size);

  // Init values for the first image
  
  Vector2   to(0, 0);
  Matrix2x2 mo;
  mo(0,0) = 1;
  mo(0,1) = 0;
  mo(1,0) = 0;
  mo(1,1) = 1;
  transforms[0] = boost::shared_ptr<vw::Transform>(new AffineTransform(mo, to));
  //transforms[0] = boost::shared_ptr<vw::Transform>(new TranslateTransform(0,0));
  bboxes    [0] = output_bbox;
  
  // This approach only works for serial pairs, if we add another type of
  //  orientation it will need to be changed.
  Matrix<double> last_transform = identity_matrix(3);
  
  for (size_t i=1; i<num_images; ++i) {

    Matrix<double> relative_transform = 
        compute_relative_transform(opt.image_files[i-1], opt.image_files[i], opt);

    image_size = file_image_size(opt.image_files[i]);

    Matrix<double> absolute_transform;
    if (i == 0) { // First transform
      absolute_transform = relative_transform;
    } else { // Chain from the last transform
      absolute_transform = last_transform * relative_transform;
    }
    last_transform = absolute_transform;
    
    Vector2   t(absolute_transform(0,2), absolute_transform(1,2));
    Matrix2x2 m;
    m(0,0) = absolute_transform(0,0);
    m(0,1) = absolute_transform(0,1);
    m(1,0) = absolute_transform(1,0);
    m(1,1) = absolute_transform(1,1);
    boost::shared_ptr<vw::Transform> tf_ptr(new AffineTransform(m, t));

    Matrix<double> inv_absolute_transform = inverse(absolute_transform);
    Vector2   ti(inv_absolute_transform(0,2), inv_absolute_transform(1,2));
    Matrix2x2 mi;
    mi(0,0) = inv_absolute_transform(0,0);
    mi(0,1) = inv_absolute_transform(0,1);
    mi(1,0) = inv_absolute_transform(1,0);
    mi(1,1) = inv_absolute_transform(1,1);
    boost::shared_ptr<vw::Transform> inv_tf_ptr(new AffineTransform(mi, ti));
    
    transforms[i] = tf_ptr; // Record transfrom from output to input

    std::cout << "relative_transform: " << relative_transform << std::endl;
    std::cout << "absolute_transform: " << absolute_transform << std::endl;

    // Update the overall output bbox with the new image added
    // TODO: Add other corners!
    Vector2 new_bot_right_corner = tf_ptr->forward(image_size);
    output_bbox.grow(new_bot_right_corner);

    std::cout << "image_size: " << image_size << std::endl;
    std::cout << "new_bot_right_corner: " << new_bot_right_corner << std::endl;
    std::cout << "Overall bbox: " << output_bbox << std::endl;

    // Update this image's bbox in output image
    BBox2f this_bbox = compute_transformed_bbox_fast(
                                BBox2i(0,0,image_size[0],image_size[1]),
                                *tf_ptr);
    this_bbox.expand(1);
    this_bbox.crop(output_bbox); // TODO: Should not be needed!
    bboxes[i] = this_bbox;
    /*
    // DEBUG Write out the entire transformed image
    write_image( "input.tif",transform(DiskImageView<float>(opt.image_files[i]), 
                                      AffineTransform(m, t),
                                      output_bbox.size()[0], output_bbox.size()[1]) );
    */
    std::cout << "This bbox: "    << bboxes[i]   << std::endl;

  } // End loop through images
  
  output_image_size = output_bbox.size();
}


/// A class to mosaic and rescale images using bilinear interpolation.
template <class T>
class ImageMosaicView: public ImageViewBase<ImageMosaicView<T> >{
private:
  std::vector<ImageViewRef<T> > const& m_images;
  std::vector<boost::shared_ptr<vw::Transform> > const& m_transforms;
  std::vector<BBox2i>          const& m_bboxes;
  int            m_blend_radius;
  Vector2i const m_output_image_size;
  double         m_output_nodata_value;

public:
  ImageMosaicView(std::vector<ImageViewRef<T> > const& images,
                  std::vector<boost::shared_ptr<vw::Transform> > const& transforms,
                  std::vector<BBox2i>          const& bboxes,
                  int      blend_radius,
                  Vector2i output_image_size, 
                  double   output_nodata_value):
    m_images(images), m_transforms(transforms),
    m_bboxes(bboxes), m_blend_radius(blend_radius),
    m_output_image_size(output_image_size),
    m_output_nodata_value(output_nodata_value){}

  typedef float pixel_type;
  typedef float result_type;
  typedef ProceduralPixelAccessor<ImageMosaicView> pixel_accessor;

  inline int32 cols  () const { return m_output_image_size[0]; }
  inline int32 rows  () const { return m_output_image_size[1]; }
  inline int32 planes() const { return 1; }

  inline pixel_accessor origin() const { return pixel_accessor( *this, 0, 0 ); }

  inline result_type operator()( double/*i*/, double/*j*/, int32/*p*/ = 0 ) const {
    vw_throw(NoImplErr() << "ImageMosaicView::operator()(...) is not implemented");
    return result_type();
  }

  typedef CropView<ImageView<result_type> > prerasterize_type;
  inline prerasterize_type prerasterize(BBox2i const& bbox) const {

    // Initialize the output tile
    ImageView<result_type> tile   (bbox.width(), bbox.height());
    ImageView<float      > weights(bbox.width(), bbox.height());
    fill(tile,    m_output_nodata_value);
    fill(weights, 0);

    // Loop through the intersecting input images and paste them in
    //  to the output image.
    for (size_t i=0; i<m_images.size(); ++i) {

      //std::cout << "i = " << i << std::endl;
      //std::cout << "bbox = " << bbox << std::endl;
      //std::cout << "m_bboxes[i] = " << m_bboxes[i] << std::endl;
      
      // Get the intersection (if any) of this image with the current bbox.
      if (!m_bboxes[i].intersects(bbox)) {
        //std::cout << "Skipping\n";
        continue;
      }
      BBox2i intersect = m_bboxes[i];
      intersect.crop(bbox);

      typedef ImageView<T> ImageT;
      typedef InterpolationView<ImageT, BilinearInterpolation> InterpT;
      
      // Find the required section of the input image.
      //BBox2f temp_bbox = compute_transformed_bbox_fast(intersect, inv_trans);
      BBox2i temp_bbox = m_transforms[i]->reverse_bbox(intersect);
      temp_bbox.expand(BilinearInterpolation::pixel_buffer);
      BBox2i input_bbox = temp_bbox;
      input_bbox.crop(bounding_box(m_images[i]));
      
      BBox2i tile_bbox = intersect - bbox.min(); // ROI of this input in the output tile

      //std::cout << "intersect = " << intersect << std::endl;
      //std::cout << "intersect.max() = " << intersect.max() << std::endl;
      //std::cout << "input_bbox = " << input_bbox << std::endl;
      //std::cout << "tile_bbox = " << tile_bbox << std::endl;

      // TODO: Clean up
      AffineTransform* temp = dynamic_cast<AffineTransform*>(m_transforms[i].get());

      // TODO: Some kind of blending?
      // --> Requires loading a larger section of the input image,
      //     calling grassfire on it, and then extracting out the section we need.

      /// This sets the extra area we work with to improve the blending weights.
      BBox2i expanded_intersect = intersect;
      expanded_intersect.expand(m_blend_radius);
      
      // Get the cropped piece of the transformed input image that we need
      ImageView<T> trans_input = crop(transform(m_images[i], *temp,
                                                ZeroEdgeExtension(),
                                                BilinearInterpolation()),
                                      expanded_intersect);
      ImageView<double> input_weights;
      //  = grassfire(notnodata(apply_mask(trans_input,0), 0));
      centerline_weights3(trans_input, input_weights);
      
      double dist = std::min(intersect.height(), intersect.width()) / 2.0;
      double denom = dist + m_blend_radius;
      
      double cutoff = (m_blend_radius/denom);//*(dist/denom);
      //std::cout << "dist = " << dist << std::endl;
      //std::cout << "cutoff = " << cutoff << std::endl;
      
      for (int r=0; r<input_weights.rows(); ++r) {
        for (int c=0; c<input_weights.cols(); ++c) {
          if (input_weights(c,r) > cutoff)
            input_weights(c,r) = cutoff;
        }
      }
      //std::string fix = "_"+num2str(i)+"_"+num2str(bbox.min()[0])+"_"+num2str(bbox.min()[1])+".tif";
      //write_image("input"+fix, apply_mask(trans_input,0));
      //write_image("weights"+fix, input_weights);
      //write_image("weights_crop"+fix, crop(input_weights, BBox2i(m_blend_radius,m_blend_radius, intersect.width(), intersect.height())));
      
      //vw_throw( NoImplErr() << "DEBUG\n" );

      // Copy that piece to the output tile, applying the mask.
      for (int r=0; r<intersect.height(); ++r) {
        for (int c=0; c<intersect.width(); ++c) {

          double weight = input_weights(c+m_blend_radius,r+m_blend_radius);
          //if (weight > cutoff)
          //  weight = cutoff;
          
          T pixel = trans_input(c+m_blend_radius,r+m_blend_radius);
          if (is_valid(pixel)) {
            float value = remove_mask(pixel);
            int o_c = c+tile_bbox.min()[0];
            int o_r = r+tile_bbox.min()[1];
            if (weights(o_c, o_r) == 0)
              tile(o_c, o_r) = value * weight;
            else
              tile(o_c, o_r) += value * weight;
            weights(o_c, o_r) += weight;
          }
        }
      } // End loop through tile intersection

      //std::cout << "input finished\n";
    } // End loop through input images

    //std::string fix = "_"+num2str(bbox.min()[0])+"_"+num2str(bbox.min()[1])+".tif";
    //write_image("tile"+fix, tile);
    //write_image("tile_weights"+fix, weights);
    
    // Normalize output by the weight.
    for (int c = 0; c < bbox.width(); c++){
      for (int r = 0; r < bbox.height(); r++){
        if ( weights(c, r) > 0 )
          tile(c, r) /= weights(c, r);
      } // End row loop
    } // End col loop
    
    //write_image("tile_norm"+fix, tile);
    
    //std::cout << "Tile finished\n";
    return prerasterize_type(tile, -bbox.min().x(), -bbox.min().y(),
                             cols(), rows() );
  } // End function prerasterize

  template <class DestT>
  inline void rasterize(DestT const& dest, BBox2i bbox) const {
    vw::rasterize(prerasterize(bbox), dest, bbox);
  }
}; // End class ImageMosaicView






// Write the image out, converting to the specified data type.
void write_selected_image_type(ImageViewRef<float> const& out_img,
                               double output_nodata_value,
                               Options const& opt) {

   // Set up our output image object
    vw_out() << "Writing: " << opt.output_image << std::endl;
    TerminalProgressCallback tpc("asp", "\t    Mosaic:");

    // Write to disk using the specified output data type.
    if (opt.output_type == "Float32") 
      vw::cartography::block_write_gdal_image(opt.output_image, out_img,
                                              output_nodata_value, opt, tpc);
    else if (opt.output_type == "Byte") 
      vw::cartography::block_write_gdal_image(opt.output_image,
                                              per_pixel_filter(out_img,
                                                               RoundAndClamp<uint8, float>()),
                                              vw::round_and_clamp<uint8>(output_nodata_value),
                                              opt, tpc);
    else if (opt.output_type == "UInt16") 
      vw::cartography::block_write_gdal_image(opt.output_image,
                                              per_pixel_filter(out_img,
                                                               RoundAndClamp<uint16, float>()),
                                              vw::round_and_clamp<uint16>(output_nodata_value),
                                              opt, tpc);
    else if (opt.output_type == "Int16") 
      vw::cartography::block_write_gdal_image(opt.output_image,
                                              per_pixel_filter(out_img,
                                                               RoundAndClamp<int16, float>()),
                                              vw::round_and_clamp<int16>(output_nodata_value),
                                              opt, tpc);

    else if (opt.output_type == "UInt32") 
      vw::cartography::block_write_gdal_image(opt.output_image,
                                              per_pixel_filter(out_img,
                                                               RoundAndClamp<uint32, float>()),
                                              vw::round_and_clamp<uint32>(output_nodata_value),
                                              opt, tpc);
    else if (opt.output_type == "Int32") 
      vw::cartography::block_write_gdal_image(opt.output_image,
                                              per_pixel_filter(out_img,
                                                               RoundAndClamp<int32, float>()),
                                              vw::round_and_clamp<int32>(output_nodata_value),
                                              opt, tpc);
    else
      vw_throw( NoImplErr() << "Unsupported output type: " << opt.output_type << ".\n" );

} // End function write_selected_image_type



void handle_arguments( int argc, char *argv[], Options& opt ) {
  po::options_description general_options("");
  general_options.add( vw::cartography::GdalWriteOptionsDescription(opt) );
  general_options.add_options()
    ("orientation", po::value(&opt.orientation)->default_value("horizontal"),
          "Choose a supported image layout from [horizontal].")
    ("overlap-width", po::value(&opt.overlap_width)->default_value(2000),
          "Select the size of the overlap region to use.")
    ("blend-radius", po::value(&opt.blend_radius)->default_value(0),
          "Size to perform blending over.  Default is the overlap width.")
    ("output-image,o", po::value(&opt.output_image)->default_value(""),
     "Specify the output image.")
    ("ot",  po::value(&opt.output_type)->default_value("Float32"),
          "Output data type. Supported types: Byte, UInt16, Int16, UInt32, Int32, Float32. If the output type is a kind of integer, values are rounded and then clamped to the limits of that type.")
    ("band", po::value(&opt.band), "Which band to use (for multi-spectral images).")
    ("input-nodata-value", po::value(&opt.input_nodata_value),
          "Nodata value to use on input; input pixel values less than or equal to this are considered invalid.")
    ("output-nodata-value", po::value(&opt.output_nodata_value),
          "Nodata value to use on output.");
 
  po::options_description positional("");
  positional.add_options()
    ("input-files", po::value(&opt.image_files));

  po::positional_options_description positional_desc;
  positional_desc.add("input-files", -1);


  std::string usage("image_mosaic <images> [options]");
  bool allow_unregistered = false;
  std::vector<std::string> unregistered;
  po::variables_map vm =
    asp::check_command_line( argc, argv, opt, general_options, general_options,
                             positional, positional_desc, usage,
                             allow_unregistered, unregistered );

  opt.has_input_nodata_value  = vm.count("input-nodata-value" );
  opt.has_output_nodata_value = vm.count("output-nodata-value");

  if ( opt.image_files.empty() )
    vw_throw( ArgumentErr() << "No images to mosaic.\n" << usage << general_options );

  if ( opt.output_image.empty() )
    vw_throw( ArgumentErr() << "Missing output image name.\n" << usage << general_options );
  
  if ( opt.blend_radius == 0 ) {
    opt.blend_radius = opt.overlap_width;
    vw_out() << "Using blend radius: " << opt.blend_radius << std::endl;
  }
  
  int min_tile_size = 2*opt.blend_radius;
  if (opt.raster_tile_size[0] < min_tile_size) {
    opt.raster_tile_size[0] = min_tile_size;
    fix_tile_multiple(opt.raster_tile_size[0]);
  }
  if (opt.raster_tile_size[1] < min_tile_size) {
    opt.raster_tile_size[1] = min_tile_size;
    fix_tile_multiple(opt.raster_tile_size[1]);
  }
  vw_out() << "Using tile size: " << opt.raster_tile_size << std::endl;
  
  
}

int main( int argc, char *argv[] ) {

  Options opt;

  //try {

    // Find command line options
    handle_arguments( argc, argv, opt );

    // Compute the transforms between all of the images on disk
    std::vector<boost::shared_ptr<vw::Transform> > transforms;
    std::vector<BBox2i>          bboxes;
    Vector2i                     output_image_size;
    compute_all_image_positions(opt, transforms, bboxes, output_image_size);

    // TODO: Handle nodata!
    // Get handles to all of the input images.
    size_t num_images = opt.image_files.size();
    std::vector<ImageViewRef<PixelMask<float> > > images(num_images);
    double nodata;
    for (size_t i=0; i<num_images; ++i) {
      // Apply a nodata mask here.
      ImageViewRef<float> temp;
      get_input_image(opt.image_files[i], opt, temp, nodata);
      images[i] = create_mask_less_or_equal(temp, nodata);
    }

    // If nodata was not provided, take one from the input images.
    double output_nodata_value = nodata;
    if (opt.has_output_nodata_value)
      output_nodata_value = opt.output_nodata_value;

    // Set up our output image object
    vw_out() << "Writing: " << opt.output_image << std::endl;
    TerminalProgressCallback tpc("asp", "\t    Mosaic:");
    ImageViewRef<float> out_img = 
        ImageMosaicView<PixelMask<float> >(images, transforms, bboxes,
                                           opt.blend_radius, output_image_size,
                                           opt.output_nodata_value);

    // Write it to disk.
    write_selected_image_type(out_img, opt.output_nodata_value, opt);

  //} ASP_STANDARD_CATCHES;
  return 0;
}
