#include "frontend_renderer.hpp"
#include "message_subclasses.hpp"

#include "../base/assert.hpp"
#include "../base/stl_add.hpp"

#include "../geometry/any_rect2d.hpp"

#include "../std/bind.hpp"
#include "../std/cmath.hpp"

namespace df
{
  FrontendRenderer::FrontendRenderer(RefPointer<ThreadsCommutator> commutator,
                                     RefPointer<OGLContextFactory> oglcontextfactory,
                                     int w, int h)
    : m_commutator(commutator)
    , m_gpuProgramManager(new GpuProgramManager())
    , m_contextFactory(oglcontextfactory)
    , m_width(w)
    , m_height(h)
  {
#ifdef DRAW_INFO
    m_tpf = 0,0;
    m_fps = 0.0;
#endif

    m_commutator->RegisterThread(ThreadsCommutator::RenderThread, this);
    RefreshProjection(w, h);
    RefreshModelView(0);
    StartThread();
  }

  FrontendRenderer::~FrontendRenderer()
  {
    StopThread();
  }

#ifdef DRAW_INFO
  void FrontendRenderer::BeforeDrawFrame()
  {
    m_frameStartTime = m_timer.ElapsedSeconds();
  }

  void FrontendRenderer::AfterDrawFrame()
  {
    m_drawedFrames++;

    double elapsed = m_timer.ElapsedSeconds();
    m_tpfs.push_back(elapsed - m_frameStartTime);

    if (elapsed > 1.0)
    {
      m_timer.Reset();
      m_fps = m_drawedFrames / elapsed;
      m_drawedFrames = 0;

      m_tpf = accumulate(m_tpfs.begin(), m_tpfs.end(), 0.0) / m_tpfs.size();

      LOG(LINFO, ("Average Fps : ", m_fps));
      LOG(LINFO, ("Average Tpf : ", m_tpf));
    }
  }
#endif

  void FrontendRenderer::AcceptMessage(RefPointer<Message> message)
  {
    switch (message->GetType())
    {

    case Message::FlushTile:
      {
        FlushTileMessage * msg = static_cast<FlushTileMessage *>(message.GetRaw());
        const GLState & state = msg->GetState();
        const TileKey & key = msg->GetKey();
        MasterPointer<VertexArrayBuffer> buffer(msg->AcceptBuffer());
        RefPointer<GpuProgram> program = m_gpuProgramManager->GetProgram(state.GetProgramIndex());
        program->Bind();
        buffer->Build(program);
        render_data_t::iterator renderIterator = m_renderData.insert(make_pair(state, buffer));
        m_tileData.insert(make_pair(key, renderIterator));
        break;
      }

    case Message::DropTiles:
      {
        CoverageUpdateDescriptor const & descr = static_cast<DropTilesMessage *>(message.GetRaw())->GetDescriptor();
        ASSERT(!descr.IsEmpty(), ());

        if (!descr.DoDropAll())
        {
          vector<TileKey> const & tilesToDrop = descr.GetTilesToDrop();
          for (size_t i = 0; i < tilesToDrop.size(); ++i)
          {
            tile_data_range_t range = m_tileData.equal_range(tilesToDrop[i]);
            for (tile_data_iter eraseIter = range.first; eraseIter != range.second; ++eraseIter)
            {
              eraseIter->second->second.Destroy();
              m_renderData.erase(eraseIter->second);
            }
            m_tileData.erase(range.first, range.second);
          }
        }
        else
          DeleteRenderData();

        break;
      }

    case Message::Resize:
      {
        ResizeMessage * rszMsg = static_cast<ResizeMessage *>(message.GetRaw());
        RefreshProjection(rszMsg->GetRect().SizeX(), rszMsg->GetRect().SizeY());
        break;
      }

    case Message::Rotate:
      {
        RotateMessage * rtMsg = static_cast<RotateMessage *>(message.GetRaw());
        RefreshModelView(rtMsg->GetDstAngle());

        ScreenBase screen(m2::RectI(0, 0, m_width, m_height),
                          m2::AnyRectD(m2::PointD(0, 0), ang::AngleD(rtMsg->GetDstAngle()),
                                       m2::RectD(0 ,0, 50, 50)));

        m_commutator->PostMessage(ThreadsCommutator::ResourceUploadThread,
                                  MovePointer<Message>(new UpdateCoverageMessage(screen)));
        break;
      }

    default:
      ASSERT(false, ());
    }
  }

  namespace
  {
    void OrthoMatrix(float * m, float left, float right, float bottom, float top, float near, float far)
    {
      memset(m, 0, 16 * sizeof(float));
      m[0]  = 2.0f / (right - left);
      m[4]  = - (right + left) / (right - left);
      m[5]  = 2.0f / (top - bottom);
      m[9]  = - (top + bottom) / (top - bottom);
      m[10] = -2.0f / (far - near);
      m[14] = - (far + near) / (far - near);
      m[15] = 1.0;
    }
  }

  void FrontendRenderer::RenderScene()
  {
#ifdef DRAW_INFO
    BeforeDrawFrame();
#endif

    GLFunctions::glViewport(0, 0, m_width, m_height);
    GLFunctions::glClearColor(0.65f, 0.65f, 0.65f, 1.f);
    GLFunctions::glClear();

    for_each(m_renderData.begin(), m_renderData.end(), bind(&FrontendRenderer::RenderPartImpl, this, _1));

#ifdef DRAW_INFO
    AfterDrawFrame();
#endif
  }

  void FrontendRenderer::RefreshProjection(int w, int h)
  {
    if (w == 0)
      w = 1;

    m_height = h;
    m_width = w;

    float aspect = h / (float)w;
    float m[4*4];

    if (w >= h)
      OrthoMatrix(m, -2.f/aspect, 2.f/aspect, -2.f, 2.f, -2.f, 2.f);
    else
      OrthoMatrix(m, -2.f, 2.f, -2.f*aspect, 2.f*aspect, -2.f, 2.f);

    m_generalUniforms.SetMatrix4x4Value("projection", m);
  }

  void FrontendRenderer::RefreshModelView(float radians)
  {
    float c = cos(radians);
    float s = sin(radians);
    float model[16] =
    {
      c,  -s,   0.0, 0.0,
      s,   c,   0.0, 0.0,
      0.0, 0.0, 1.0, 0.0,
      0.0, 0.0, 0.0, 1.0
    };

    m_generalUniforms.SetMatrix4x4Value("modelView", model);
  }

  void FrontendRenderer::RenderPartImpl(pair<const GLState, MasterPointer<VertexArrayBuffer> > & node)
  {
    RefPointer<GpuProgram> program = m_gpuProgramManager->GetProgram(node.first.GetProgramIndex());

    program->Bind();
    ApplyState(node.first, program);
    ApplyUniforms(m_generalUniforms, program);

    node.second->Render();
  }

  void FrontendRenderer::StartThread()
  {
    m_selfThread.Create(this);
  }

  void FrontendRenderer::StopThread()
  {
    IRoutine::Cancel();
    CloseQueue();
    m_selfThread.Join();
  }

  void FrontendRenderer::ThreadMain()
  {
    OGLContext * context = m_contextFactory->getDrawContext();
    context->makeCurrent();

    while (!IsCancelled())
    {
      ProcessSingleMessage(false);
      context->setDefaultFramebuffer();
      RenderScene();
      context->present();
    }

    ReleaseResources();
  }

  void FrontendRenderer::ReleaseResources()
  {
    DeleteRenderData();
    m_gpuProgramManager.Destroy();
  }

  void FrontendRenderer::Do()
  {
    ThreadMain();
  }

  void FrontendRenderer::DeleteRenderData()
  {
    m_tileData.clear();
    GetRangeDeletor(m_renderData, MasterPointerDeleter())();
  }
}
