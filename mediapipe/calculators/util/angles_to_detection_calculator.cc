// Copyright 2020 Lisandro Bravo.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <memory>

#include "mediapipe/calculators/util/angles_to_detection_calculator.pb.h"
#include "mediapipe/framework/calculator_framework.h"
#include "mediapipe/framework/formats/detection.pb.h"
#include "tensorflow/lite/interpreter.h"
#include "mediapipe/framework/port/ret_check.h"

namespace mediapipe {

typedef std::vector<Detection> Detections; 

namespace {

constexpr char kDetectionTag[] = "DETECTIONS";
constexpr char kTfLiteFloat32[] = "TENSORS"; 

}  // namespace

// Converts Angles to Detection proto. 
//
// If option queue_size is set , it will put each new 
// detection in a FIFO of max size queue_size  
// and will return the class corresponding to the 
// highest occcurence in the FIFO, usefull to stabilize detections
// eliminating spurious missclasifications 
//
// Input:
//  TENSOR: A Vector of TfLiteTensor of type kTfLiteFloat32 with the confidence score
//          for each static gesture.
//
// Output:
//   DETECTION: A Detection proto.
//
// Example config:
// node {
//   calculator: "AnglesToDetectionCalculator"
//   input_stream: ""TENSORS:tensors"
//   output_stream: "DETECTIONS:detections"
// }

class AnglesToDetectionCalculator : public CalculatorBase {
 public:
  static ::mediapipe::Status GetContract(CalculatorContract* cc);
  ::mediapipe::Status Open(CalculatorContext* cc) override;

  ::mediapipe::Status Process(CalculatorContext* cc) override;

 private:
  typedef struct {
    float score_; 
    int label_;
    } inValues_t ;
 
  std::vector<inValues_t> inferenceQueue;
  decltype(Timestamp().Seconds()) startingGestureTime;
  ::mediapipe::AnglesToDetectionCalculatorOptions options_;
  void mostFrequent(inValues_t &currentInference, 
                    decltype(Timestamp().Seconds()) currGestureTime);
  
};
REGISTER_CALCULATOR(AnglesToDetectionCalculator);

::mediapipe::Status AnglesToDetectionCalculator::GetContract(
    CalculatorContract* cc) {
  RET_CHECK(cc->Inputs().HasTag(kTfLiteFloat32));
  //RET_CHECK(cc->Outputs().HasTag(kDetectionTag));
  // TODO: Also support converting Landmark to Detection.
  cc->Inputs()
      .Tag(kTfLiteFloat32) 
      .Set<std::vector<TfLiteTensor>>();
  //cc->Outputs().Tag(kDetectionTag).Set<Detection>();
  cc->Outputs().Index(0).Set<Detections>();

  return ::mediapipe::OkStatus();
}

::mediapipe::Status AnglesToDetectionCalculator::Open(
    CalculatorContext* cc) {
  cc->SetOffset(TimestampDiff(0));

  options_ = cc->Options<::mediapipe::AnglesToDetectionCalculatorOptions>();
  return ::mediapipe::OkStatus();
}

::mediapipe::Status AnglesToDetectionCalculator::Process(
    CalculatorContext* cc) {
  RET_CHECK(!cc->Inputs().Tag(kTfLiteFloat32).IsEmpty());

  const auto& input_tensors =
      cc->Inputs().Tag(kTfLiteFloat32).Get<std::vector<TfLiteTensor>>();
  // TODO: Add option to specify which tensor to take 
  const TfLiteTensor* raw_tensor = &input_tensors[0];
  const float* raw_floats = raw_tensor->data.f;
  
  //std::cout  << raw_tensor->name;
  inValues_t currentInference; 
  

  for(int i=0;i<raw_tensor->dims->data[1]; i++){
    if(raw_floats[i]>currentInference.score_){
        currentInference.score_=raw_floats[i];
        currentInference.label_=i;
    }  
  }

  if(options_.queue_size()>1) 
    mostFrequent(currentInference, cc->InputTimestamp().Seconds());
  
  
  auto* output_detections = new Detections();
  output_detections->reserve(1);
  auto detection = absl::make_unique<Detection>();
  detection->add_score(currentInference.score_);
  detection->add_label_id(currentInference.label_);
  
  LocationData location_data;
  location_data.set_format(LocationData::BOUNDING_BOX);
  location_data.mutable_bounding_box()->set_xmin(450);
  location_data.mutable_bounding_box()->set_ymin(450);
  location_data.mutable_bounding_box()->set_width(200);
  location_data.mutable_bounding_box()->set_height(20);
  *(detection->mutable_location_data()) = location_data;

  
  
  output_detections->push_back(*detection);

  cc->Outputs()
      .Index(0)
      .Add(output_detections, cc->InputTimestamp());

  return ::mediapipe::OkStatus();
}

void AnglesToDetectionCalculator::mostFrequent(inValues_t &currentInference,
                                               decltype(Timestamp().Seconds()) currGestureTime){
  struct attr
  {
    decltype(currentInference.score_) sumScores;
    int32 counts;
  } inferenceAttr={};
  
  struct highestOccurrence_t{
    int32 label;
    int32 count;
  } highestOccurrence = {};

  std::map<int,attr> trackInferences; //label, score, count
  std::map<int,attr>::iterator it;

  inferenceQueue.emplace_back(currentInference);

  if(options_.has_queue_time_out_s() && (startingGestureTime)){
    if((currGestureTime - 
        startingGestureTime) >= options_.queue_time_out_s()){
      inferenceQueue.clear();    
    }
  }  
  startingGestureTime = currGestureTime;


  if(inferenceQueue.size()>=options_.queue_size()){
    inferenceQueue.erase(inferenceQueue.begin());

    for(auto cInference: inferenceQueue){
      
       
      it = trackInferences.find(cInference.label_);
      if(it == trackInferences.end()) {
        inferenceAttr.counts=1;
        inferenceAttr.sumScores=cInference.score_;
        trackInferences.emplace(cInference.label_,inferenceAttr);
      }
      else{
        trackInferences[cInference.label_].counts++;
        trackInferences[cInference.label_].sumScores+=cInference.score_;
      }
      
      if(trackInferences[cInference.label_].counts > highestOccurrence.count){ 
        highestOccurrence.label = cInference.label_;
        highestOccurrence.count = trackInferences[cInference.label_].counts;
      }

      //  std::cout << "\n3:" << std::to_string(i)
      //          << "\t" << std::to_string(inferenceQueue[i].label_)
      //          //<< "\t" << std::to_string(c_ference.label_)
      //          << "\t" << std::to_string(trackInferences[inferenceQueue[i].label_].counts)
      //          << "\t" << std::to_string(highestOccurrence.label);
    
    }
  
    currentInference.label_=highestOccurrence.label;
    currentInference.score_= trackInferences[highestOccurrence.label].sumScores / 
                          highestOccurrence.count;

    trackInferences.clear();  
  }  
}

}  // namespace mediapipe
