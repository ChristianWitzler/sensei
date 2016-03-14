#include "Slice.h"
#include "Utilities.h"

#include <timer/Timer.h>

#include <vtkCPDataDescription.h>
#include <vtkCPInputDataDescription.h>
#include <vtkObjectFactory.h>
#include <vtkPVDataInformation.h>
#include <vtkPVDataSetAttributesInformation.h>
#include <vtkPVTrivialProducer.h>
#include <vtkSMProperty.h>
#include <vtkSMProxyListDomain.h>
#include <vtkSMTransferFunctionProxy.h>
#include <vtkPVArrayInformation.h>
#include <vtkMultiProcessController.h>
#include <vtkCommunicator.h>

#include <sstream>

namespace sensei
{
namespace catalyst
{
class Slice::vtkInternals
{
public:
  vtkSmartPointer<vtkSMSourceProxy> TrivialProducer;
  vtkSmartPointer<vtkSMSourceProxy> Slice;
  vtkSmartPointer<vtkSMProxy> SlicePlane;
  vtkSmartPointer<vtkSMViewProxy> RenderView;
  vtkSmartPointer<vtkSMProxy> SliceRepresentation;
  double Origin[3];
  double Normal[3];
  bool PipelineCreated;
  int ColorAssociation;
  std::string ColorArrayName;
  bool AutoCenter;

  std::string ImageFileName;
  int ImageSize[2];

  vtkInternals() : PipelineCreated(false), ColorAssociation(0), AutoCenter(true)
  {
  this->ImageSize[0] = this->ImageSize[1] = 800;
  }

  bool EnableRendering() const
    {
    return !this->ImageFileName.empty();
    }

  void UpdatePipeline(vtkDataObject* data, int timestep, double time)
    {
    if (!this->PipelineCreated)
      {
      this->TrivialProducer = catalyst::CreatePipelineProxy("sources", "PVTrivialProducer");
      vtkPVTrivialProducer *tp = vtkPVTrivialProducer::SafeDownCast(
        this->TrivialProducer->GetClientSideObject());
      tp->SetOutput(data, time);

      this->Slice = catalyst::CreatePipelineProxy("filters", "Cut", this->TrivialProducer);
      vtkSMProxyListDomain* pld = vtkSMProxyListDomain::SafeDownCast(
        this->Slice->GetProperty("CutFunction")->FindDomain("vtkSMProxyListDomain"));
      this->SlicePlane = pld->FindProxy("implicit_functions", "Plane");
      vtkSMPropertyHelper(this->Slice, "CutFunction").Set(this->SlicePlane);
      this->Slice->UpdateVTKObjects();

      if (this->EnableRendering())
        {
        this->RenderView = catalyst::CreateViewProxy("views", "RenderView");
        vtkSMPropertyHelper(this->RenderView, "ShowAnnotation", true).Set(1);
        vtkSMPropertyHelper(this->RenderView, "ViewTime").Set(time);
        vtkSMPropertyHelper(this->RenderView, "ViewSize").Set(this->ImageSize, 2);
        this->RenderView->UpdateVTKObjects();

        this->SliceRepresentation = catalyst::Show(this->Slice, this->RenderView);
        }

      this->PipelineCreated = true;
      }
    else
      {
      vtkPVTrivialProducer *tp = vtkPVTrivialProducer::SafeDownCast(
        this->TrivialProducer->GetClientSideObject());
      tp->SetOutput(data, time);
      }

    vtkMultiProcessController* controller = vtkMultiProcessController::GetGlobalController();
    if (this->AutoCenter)
      {
      this->TrivialProducer->UpdatePipeline(time);
      double bds[6];
      this->TrivialProducer->GetDataInformation()->GetBounds(bds);
      bds[0] *=-1; bds[2] *= -1; bds[4] *= -1;

      double gbds[6];
      controller->AllReduce(bds, gbds, 6, vtkCommunicator::MAX_OP);
      gbds[0] *=-1; gbds[2] *= -1; gbds[4] *= -1;

      double center[3] = {
        (gbds[0] + gbds[1]) / 2.0,
        (gbds[2] + gbds[3]) / 2.0,
        (gbds[4] + gbds[5]) / 2.0};
      vtkSMPropertyHelper(this->SlicePlane, "Origin").Set(center, 3);
      }
    else
      {
      vtkSMPropertyHelper(this->SlicePlane, "Origin").Set(this->Origin, 3);
      }
    vtkSMPropertyHelper(this->SlicePlane, "Normal").Set(this->Normal, 3);
    this->SlicePlane->UpdateVTKObjects();
    this->Slice->UpdatePipeline(time);

    if (this->EnableRendering())
      {
      vtkSMPropertyHelper(this->RenderView, "ViewTime").Set(time);
      this->RenderView->UpdateVTKObjects();
      vtkSMPVRepresentationProxy::SetScalarColoring(
        this->SliceRepresentation, this->ColorArrayName.c_str(), this->ColorAssociation);
      if (vtkSMPVRepresentationProxy::GetUsingScalarColoring(this->SliceRepresentation))
        {
        double range[2] = {VTK_DOUBLE_MAX, VTK_DOUBLE_MIN}, grange[2];
        if (vtkPVArrayInformation* ai = vtkSMPVRepresentationProxy::GetArrayInformationForColorArray(
            this->SliceRepresentation))
          {
          ai->GetComponentRange(-1, range);
          }

        range[0] *= -1; // make range[0] negative to simplify reduce.
        controller->AllReduce(range, grange, 2, vtkCommunicator::MAX_OP);
        grange[0] *= -1;
        vtkSMTransferFunctionProxy::RescaleTransferFunction(
          vtkSMPropertyHelper(this->SliceRepresentation, "LookupTable").GetAsProxy(), grange[0], grange[1]);
        }
      vtkSMRenderViewProxy::SafeDownCast(this->RenderView)->ResetCamera();
      // this->RenderView->StillRender();
      std::string filename = this->ImageFileName;

      // replace "%ts" with timestep in filename
      std::ostringstream ts_stream;
      ts_stream << timestep;
      std::string::size_type pos = filename.find("%ts");
      while (pos != std::string::npos)
        {
        filename.replace(pos, 3, ts_stream.str());
        pos = filename.find("%ts");
        }
      // replace "%t" with time in filename
      std::ostringstream t_stream;
      t_stream << time;
      pos = filename.find("%t");
      while (pos != std::string::npos)
        {
        filename.replace(pos, 2, t_stream.str());
        pos = filename.find("%t");
        }
      this->RenderView->WriteImage(filename.c_str(), "vtkPNGWriter", 1);
      }
    }

};

vtkStandardNewMacro(Slice);
//----------------------------------------------------------------------------
Slice::Slice()
{
  this->Internals = new vtkInternals();
}

//----------------------------------------------------------------------------
Slice::~Slice()
{
  vtkInternals& internals = (*this->Internals);
  catalyst::DeletePipelineProxy(internals.Slice);
  catalyst::DeletePipelineProxy(internals.TrivialProducer);
  delete this->Internals;
}

//----------------------------------------------------------------------------
void Slice::SetSliceOrigin(double x, double y, double z)
{
  vtkInternals& internals = (*this->Internals);
  internals.Origin[0] = x;
  internals.Origin[1] = y;
  internals.Origin[2] = z;
}

//----------------------------------------------------------------------------
void Slice::SetSliceNormal(double x, double y, double z)
{
  vtkInternals& internals = (*this->Internals);
  internals.Normal[0] = x;
  internals.Normal[1] = y;
  internals.Normal[2] = z;
}

//----------------------------------------------------------------------------
void Slice::SetAutoCenter(bool val)
{
  vtkInternals& internals = (*this->Internals);
  internals.AutoCenter = val;
}

//----------------------------------------------------------------------------
void Slice::ColorBy(int association, const std::string& arrayname)
{
  vtkInternals& internals = (*this->Internals);
  internals.ColorArrayName = arrayname;
  internals.ColorAssociation = association;
}

//----------------------------------------------------------------------------
void Slice::SetImageParameters(const std::string& filename, int width, int height)
{
  vtkInternals& internals = (*this->Internals);
  internals.ImageFileName = filename;
  internals.ImageSize[0] = width;
  internals.ImageSize[1] = height;
}

//----------------------------------------------------------------------------
int Slice::RequestDataDescription(vtkCPDataDescription* dataDesc)
{
  dataDesc->GetInputDescription(0)->GenerateMeshOn();
  dataDesc->GetInputDescription(0)->AllFieldsOn();
  return 1;
}

//----------------------------------------------------------------------------
int Slice::CoProcess(vtkCPDataDescription* dataDesc)
{
  timer::MarkEvent mark("catalyst::slice");
  vtkInternals& internals = (*this->Internals);
  internals.UpdatePipeline(dataDesc->GetInputDescription(0)->GetGrid(),
    dataDesc->GetTimeStep(), dataDesc->GetTime());
  return 1;
}

//----------------------------------------------------------------------------
int Slice::Finalize()
{
  return 1;
}

//----------------------------------------------------------------------------
void Slice::PrintSelf(ostream& os, vtkIndent indent)
{
  this->Superclass::PrintSelf(os, indent);
}

} // catalyst
} // sensei