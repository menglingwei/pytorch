/**
 * Copyright (c) 2016-present, Facebook, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <opencv2/opencv.hpp>
#include <fstream>

#include "caffe2/core/common.h"
#include "caffe2/core/db.h"
#include "caffe2/core/init.h"
#include "caffe2/core/logging.h"
#include "caffe2/core/timer.h"
#include "caffe2/proto/caffe2_pb.h"
#include "caffe2/utils/proto_utils.h"
#include "caffe2/utils/string_utils.h"

C10_DEFINE_bool(color, true, "If set, load images in color.");
C10_DEFINE_string(
    crop,
    "-1,-1",
    "The center cropped hight and width. If the value is less than zero, "
    "it is not cropped.");
C10_DEFINE_string(input_images, "", "Comma separated images");
C10_DEFINE_string(input_image_file, "", "The file containing imput images");
C10_DEFINE_string(output_tensor, "", "The output tensor file in NCHW");
C10_DEFINE_string(
    preprocess,
    "",
    "Options to specify the preprocess routines. The available options are "
    "subtract128, normalize, mean, std, bgrtorgb. If multiple steps are provided, they "
    "are separated by comma (,) in sequence.");
C10_DEFINE_string(
    report_time,
    "",
    "Report the conversion stage time to screen. "
    "The format of the string is <type>|<identifier>. "
    "The valid type is 'json'. "
    "The valid identifier is nothing or an identifer that prefix every line");
C10_DEFINE_int(scale, 256, "Scale the shorter edge to the given value.");
C10_DEFINE_bool(text_output, false, "Write the output in text format.");
C10_DEFINE_bool(warp, false, "If warp is set, warp the images to square.");

namespace caffe2 {

void reportTime(
    std::string type,
    double ts,
    std::string metric,
    std::string unit) {
  if (FLAGS_report_time == "") {
    return;
  }
  vector<string> s = caffe2::split('|', FLAGS_report_time);
  assert(s[0] == "json");
  std::string identifier = "";
  if (s.size() > 1) {
    identifier = s[1];
  }
  std::cout << identifier << "{\"type\": \"" << type << "\", \"value\": " << ts
            << ", \"metric\": \"" << metric << "\", \"unit\": \"" << unit
            << "\"}" << std::endl;
}

cv::Mat resizeImage(cv::Mat& img) {
  if (FLAGS_scale <= 0) {
    return img;
  }
  cv::Mat resized_img;
  int scaled_width, scaled_height;
  if (img.rows > img.cols) {
    scaled_width = FLAGS_scale;
    scaled_height = static_cast<float>(img.rows) * FLAGS_scale / img.cols;
  } else {
    scaled_height = FLAGS_scale;
    scaled_width = static_cast<float>(img.cols) * FLAGS_scale / img.rows;
  }
  cv::resize(
      img,
      resized_img,
      cv::Size(scaled_width, scaled_height),
      0,
      0,
      cv::INTER_LINEAR);
  return resized_img;
}

cv::Mat cropToRec(cv::Mat& img, int& height, int& width) {
  // Crop image to square
  if ((height > 0) && (width > 0) &&
      ((img.rows != height) || (img.cols != width))) {
    cv::Mat cropped_img, cimg;
    cv::Rect roi;
    roi.x = int((img.cols - width) / 2);
    roi.y = int((img.rows - height) / 2);
    roi.x = roi.x < 0 ? 0 : roi.x;
    roi.y = roi.y < 0 ? 0 : roi.y;
    width = width > img.cols ? img.cols : width;
    height = height > img.rows ? img.rows : height;
    roi.width = width;
    roi.height = height;
    assert(
        0 <= roi.x && 0 <= roi.width && roi.x + roi.width <= img.cols &&
        0 <= roi.y && 0 <= roi.height && roi.y + roi.height <= img.rows);
    cropped_img = img(roi);
    // Make the image in continuous space in memory
    cimg = cropped_img.clone();
    return cimg;
  } else {
    return img;
  }
}

std::vector<float> convertToVector(cv::Mat& img) {
  std::vector<float> normalize(3, 1);
  std::vector<float> mean(3, 0);
  std::vector<float> std(3, 1);
  bool bgrtorgb = false;
  int size = img.cols * img.rows;
  vector<string> steps = caffe2::split(',', FLAGS_preprocess);
  for (int i = 0; i < steps.size(); i++) {
    auto step = steps[i];
    if (step == "subtract128") {
      mean = {128, 128, 128};
      std = {1, 1, 1};
      normalize = {1, 1, 1};
    } else if (step == "normalize") {
      normalize = {255, 255, 255};
    } else if (step == "mean") {
      mean = {0.406f, 0.456f, 0.485f};
    } else if (step == "std") {
      std = {0.225f, 0.224f, 0.229f};
    } else if (step == "bgrtorgb") {
      bgrtorgb = true;
    } else {
      CAFFE_ENFORCE(
          false,
          "Unsupported preprocess step. The supported steps are: subtract128, "
          "normalize,mean, std, swaprb.");
    }
  }

  int C = FLAGS_color ? 3 : 1;
  int total_size = C * size;
  std::vector<float> values(total_size);
  if (C == 1) {
    cv::MatIterator_<uchar> it, end;
    int idx = 0;
    for (it = img.begin<uchar>(), end = img.end<uchar>(); it != end; ++it) {
      values[idx++] = (*it / normalize[0] - mean[0]) / std[0];
    }
  } else {
    int i = 0;
    cv::MatIterator_<cv::Vec3b> it, end;
    int b = bgrtorgb ? 2 : 0;
    int g = 1;
    int r = bgrtorgb ? 0 : 2;
    for (it = img.begin<cv::Vec3b>(), end = img.end<cv::Vec3b>(); it != end;
         ++it, i++) {
      values[i] = (((*it)[b] / normalize[0] - mean[0]) / std[0]);
      int offset = size + i;
      values[offset] = (((*it)[g] / normalize[1] - mean[1]) / std[1]);
      offset = size + offset;
      values[offset] = (((*it)[r] / normalize[2] - mean[2]) / std[2]);
    }
  }
  return values;
}

std::vector<float> convertOneImage(
    std::string& filename,
    int& height,
    int& width) {
  assert(filename[0] != '~');

  std::cout << "Converting " << filename << std::endl;

  // Load image
  cv::Mat img = cv::imread(
#if CV_MAJOR_VERSION <= 3
      filename, FLAGS_color ? CV_LOAD_IMAGE_COLOR : CV_LOAD_IMAGE_GRAYSCALE);
#else
      filename, FLAGS_color ? cv::IMREAD_COLOR : cv::IMREAD_GRAYSCALE);
#endif
  caffe2::Timer timer;
  timer.Start();

  // Resize image
  cv::Mat resized_img = resizeImage(img);
  vector<string> sizes = caffe2::split(',', FLAGS_crop);
  height = std::stoi(sizes[0]);
  width = std::stoi(sizes[1]);
  if ((height <= 0) || (width <= 0)) {
    height = resized_img.rows;
    width = resized_img.cols;
  }
  cv::Mat crop = cropToRec(resized_img, height, width);
  // Assert we don't have to deal with alignment
  DCHECK(crop.isContinuous());
  assert(crop.rows == height);
  assert(crop.rows == width);
  std::vector<float> one_image_values = convertToVector(crop);
  double ts = timer.MicroSeconds();
  reportTime("image_preprocess", ts, "convert", "us");
  return one_image_values;
}

void convertImages() {
  vector<string> file_names;
  if (FLAGS_input_images != "") {
    file_names = caffe2::split(',', FLAGS_input_images);
  } else if (FLAGS_input_image_file != "") {
    std::ifstream infile(FLAGS_input_image_file);
    std::string line;
    while (std::getline(infile, line)) {
      vector<string> file_name = caffe2::split(',', line);
      string name;
      if (file_name.size() == 3) {
        name = file_name[2];
      } else {
        name = line;
      }
      file_names.push_back(name);
    }
  } else {
    assert(false);
  }
  std::vector<std::vector<float>> values;
  int C = FLAGS_color ? 3 : 1;
  int height = -1;
  int width = -1;
  for (int i = 0; i < file_names.size(); i++) {
    int one_height, one_width;
    std::vector<float> one_image_values =
        convertOneImage(file_names[i], one_height, one_width);
    if (height < 0 && width < 0) {
      height = one_height;
      width = one_width;
    } else {
      assert(height == one_height);
      assert(width == one_width);
    }
    values.push_back(one_image_values);
  }

  caffe2::Timer timer;
  timer.Start();

  TensorProtos protos;
  TensorProto* data;
  data = protos.add_protos();
  data->set_data_type(TensorProto::FLOAT);
  data->add_dims(values.size());
  data->add_dims(C);
  data->add_dims(height);
  data->add_dims(width);

  // Not optimized
  for (int i = 0; i < values.size(); i++) {
    assert(values[i].size() == C * height * width);
    for (int j = 0; j < values[i].size(); j++) {
      data->add_float_data(values[i][j]);
    }
  }
  double ts = timer.MicroSeconds();
  reportTime("image_preprocess", ts, "pack", "us");

  if (FLAGS_text_output) {
    caffe2::WriteProtoToTextFile(protos, FLAGS_output_tensor);
  } else {
    caffe2::WriteProtoToBinaryFile(protos, FLAGS_output_tensor);
  }
}

} // namespace caffe2

int main(int argc, char** argv) {
  caffe2::GlobalInit(&argc, &argv);
  caffe2::convertImages();
  return 0;
}
