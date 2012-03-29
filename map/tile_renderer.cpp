#include "../base/SRC_FIRST.hpp"

#include "tile_renderer.hpp"
#include "window_handle.hpp"

#include "../yg/internal/opengl.hpp"
#include "../yg/render_state.hpp"
#include "../yg/rendercontext.hpp"
#include "../yg/base_texture.hpp"
#include "../yg/packets_queue.hpp"

#include "../std/bind.hpp"

#include "../indexer/scales.hpp"

#include "../base/logging.hpp"
#include "../base/condition.hpp"
#include "../base/shared_buffer_manager.hpp"

TileRenderer::TileRenderer(
    string const & skinName,
    unsigned executorsCount,
    yg::Color const & bgColor,
    RenderPolicy::TRenderFn const & renderFn,
    shared_ptr<yg::gl::RenderContext> const & primaryRC,
    shared_ptr<yg::ResourceManager> const & rm,
    double visualScale,
    yg::gl::PacketsQueue ** packetsQueues
  ) : m_queue(executorsCount),
      m_renderFn(renderFn),
      m_skinName(skinName),
      m_bgColor(bgColor),
      m_sequenceID(0),
      m_isExiting(false),
      m_isPaused(false)
{
  m_resourceManager = rm;
  m_primaryContext = primaryRC;

  m_threadData.resize(m_queue.ExecutorsCount());

  LOG(LINFO, ("initializing ", m_queue.ExecutorsCount(), " rendering threads"));

  int tileWidth = m_resourceManager->params().m_renderTargetTexturesParams.m_texWidth;
  int tileHeight = m_resourceManager->params().m_renderTargetTexturesParams.m_texHeight;

  for (unsigned i = 0; i < m_threadData.size(); ++i)
  {
    DrawerYG::params_t params;

    params.m_resourceManager = m_resourceManager;
    params.m_frameBuffer = make_shared_ptr(new yg::gl::FrameBuffer());

    params.m_glyphCacheID = m_resourceManager->renderThreadGlyphCacheID(i);
    params.m_threadID = i;
    params.m_visualScale = visualScale;
    params.m_skinName = m_skinName;
    if (packetsQueues != 0)
      params.m_renderQueue = packetsQueues[i];
    params.m_doUnbindRT = false;
    params.m_isSynchronized = false;
  /*  params.m_isDebugging = true;
    params.m_drawPathes = false ;
    params.m_drawAreas = false;
    params.m_drawTexts = false; */

    m_threadData[i].m_drawerParams = params;
    m_threadData[i].m_drawer = 0;

    if (!packetsQueues)
      m_threadData[i].m_renderContext = m_primaryContext->createShared();

    m_threadData[i].m_dummyRT = m_resourceManager->createRenderTarget(2, 2);
    m_threadData[i].m_depthBuffer = make_shared_ptr(new yg::gl::RenderBuffer(tileWidth, tileHeight, true));
  }

  m_queue.AddInitCommand(bind(&TileRenderer::InitializeThreadGL, this, _1));
  m_queue.AddFinCommand(bind(&TileRenderer::FinalizeThreadGL, this, _1));

  m_queue.Start();
}

TileRenderer::~TileRenderer()
{
  m_isExiting = true;

  m_queue.Cancel();

  for (size_t i = 0; i < m_threadData.size(); ++i)
    if (m_threadData[i].m_drawer)
      delete m_threadData[i].m_drawer;
}

void TileRenderer::InitializeThreadGL(core::CommandsQueue::Environment const & env)
{
  ThreadData & threadData = m_threadData[env.threadNum()];

  int tileWidth = m_resourceManager->params().m_renderTargetTexturesParams.m_texWidth;
  int tileHeight = m_resourceManager->params().m_renderTargetTexturesParams.m_texHeight;

  if (threadData.m_renderContext)
    threadData.m_renderContext->makeCurrent();
  threadData.m_drawer = new DrawerYG(threadData.m_drawerParams);
  threadData.m_drawer->onSize(tileWidth, tileHeight);
  threadData.m_drawer->screen()->setDepthBuffer(threadData.m_depthBuffer);
}

void TileRenderer::FinalizeThreadGL(core::CommandsQueue::Environment const & env)
{
  ThreadData & threadData = m_threadData[env.threadNum()];

  if (threadData.m_renderContext)
    threadData.m_renderContext->endThreadDrawing();
}

void TileRenderer::ReadPixels(yg::gl::PacketsQueue * glQueue, core::CommandsQueue::Environment const & env)
{
  ThreadData & threadData = m_threadData[env.threadNum()];

  DrawerYG * drawer = threadData.m_drawer;

  if (glQueue)
  {
    glQueue->processFn(bind(&TileRenderer::ReadPixels, this, (yg::gl::PacketsQueue*)0, ref(env)), true);
    return;
  }

  if (!env.isCancelled())
  {
    unsigned tileWidth = m_resourceManager->params().m_renderTargetTexturesParams.m_texWidth;
    unsigned tileHeight = m_resourceManager->params().m_renderTargetTexturesParams.m_texHeight;

    shared_ptr<vector<unsigned char> > buf = SharedBufferManager::instance().reserveSharedBuffer(tileWidth * tileHeight * 4);
    drawer->screen()->finish(true);
    drawer->screen()->readPixels(m2::RectU(0, 0, tileWidth, tileHeight), &(buf->at(0)), true);
    SharedBufferManager::instance().freeSharedBuffer(tileWidth * tileHeight * 4, buf);
  }
}

void TileRenderer::DrawTile(core::CommandsQueue::Environment const & env,
                           Tiler::RectInfo const & rectInfo,
                           int sequenceID)
{
  if (m_isPaused)
    return;

  /// commands from the previous sequence are ignored
  if (sequenceID < m_sequenceID)
    return;

  if (HasTile(rectInfo))
    return;

  ThreadData & threadData = m_threadData[env.threadNum()];

  yg::gl::PacketsQueue * glQueue = threadData.m_drawerParams.m_renderQueue;

  DrawerYG * drawer = threadData.m_drawer;

  ScreenBase frameScreen;

  unsigned tileWidth = m_resourceManager->params().m_renderTargetTexturesParams.m_texWidth;
  unsigned tileHeight = m_resourceManager->params().m_renderTargetTexturesParams.m_texHeight;

  m2::RectI renderRect(1, 1, tileWidth - 1, tileHeight - 1);

  frameScreen.OnSize(renderRect);

  shared_ptr<PaintEvent> paintEvent = make_shared_ptr(new PaintEvent(drawer, &env));

  my::Timer timer;

  shared_ptr<yg::gl::BaseTexture> tileTarget = m_resourceManager->renderTargetTextures()->Reserve();

  if (m_resourceManager->renderTargetTextures()->IsCancelled())
    return;

  StartTile(rectInfo);

  drawer->screen()->setRenderTarget(tileTarget);

  shared_ptr<yg::InfoLayer> tileInfoLayer(new yg::InfoLayer());
  tileInfoLayer->setCouldOverlap(true);

  drawer->screen()->setInfoLayer(tileInfoLayer);

  /// ensuring, that the render target is not bound as a texture

  threadData.m_dummyRT->makeCurrent(glQueue);

  drawer->beginFrame();
  drawer->clear(yg::Color(m_bgColor.r, m_bgColor.g, m_bgColor.b, 0));
  drawer->screen()->setClipRect(renderRect);
  drawer->clear(m_bgColor);

/*  drawer->clear(yg::Color(rand() % 32 + 128, rand() % 64 + 128, rand() % 32 + 128, 255));

  std::stringstream out;
  out << rectInfo.m_y << ", " << rectInfo.m_x << ", " << rectInfo.m_tileScale;

  drawer->screen()->drawText(yg::FontDesc(12, yg::Color(0, 0, 0, 255), true),
                             renderRect.Center(),
                             yg::EPosCenter,
                             out.str().c_str(),
                             0,
                             false);*/
  frameScreen.SetFromRect(m2::AnyRectD(rectInfo.m_rect));

  m2::RectD selectRect;
  m2::RectD clipRect;

  double const inflationSize = 24 * drawer->VisualScale();
  frameScreen.PtoG(m2::Inflate(m2::RectD(renderRect), inflationSize, inflationSize), clipRect);
  frameScreen.PtoG(m2::RectD(renderRect), selectRect);

  m_renderFn(
        paintEvent,
        frameScreen,
        selectRect,
        clipRect,
        min(scales::GetUpperScale(), rectInfo.m_tileScale),
        rectInfo.m_tileScale <= scales::GetUpperScale()
        );

  drawer->endFrame();

  drawer->screen()->resetInfoLayer();

  // performing some computational task
  // to give this thread some time to realize
  // that it could be cancelled

  /// filter out the overlay elements that are out of the bound rect for the tile
  if (!env.isCancelled())
    tileInfoLayer->clip(renderRect);

  ReadPixels(glQueue, env);
  drawer->screen()->finish();

  drawer->screen()->unbindRenderTarget();

  if (!env.isCancelled())
  {
    if (glQueue)
      glQueue->completeCommands();
  }
  else
  {
    if (!m_isExiting)
    {
      if (glQueue)
        glQueue->cancelCommands();
    }
  }

  FinishTile(rectInfo);

  if (env.isCancelled())
  {
    if (!m_isExiting)
      m_resourceManager->renderTargetTextures()->Free(tileTarget);
  }
  else
    AddTile(rectInfo, Tile(tileTarget,
                           tileInfoLayer,
                           frameScreen,
                           rectInfo,
                           0,
                           paintEvent->isEmptyDrawing()));
}

void TileRenderer::AddCommand(Tiler::RectInfo const & rectInfo, int sequenceID, core::CommandsQueue::Chain const & afterTileFns)
{
  SetSequenceID(sequenceID);

  core::CommandsQueue::Chain chain;
  chain.addCommand(bind(&TileRenderer::DrawTile, this, _1, rectInfo, sequenceID));
  chain.addCommand(afterTileFns);

  m_queue.AddCommand(chain);
}

void TileRenderer::CancelCommands()
{
  m_queue.CancelCommands();
}

void TileRenderer::ClearCommands()
{
  m_queue.Clear();
}

void TileRenderer::SetSequenceID(int sequenceID)
{
  m_sequenceID = sequenceID;
}

TileCache & TileRenderer::GetTileCache()
{
  return m_tileCache;
}

void TileRenderer::WaitForEmptyAndFinished()
{
  m_queue.Join();
}

bool TileRenderer::HasTile(Tiler::RectInfo const & rectInfo)
{
  TileCache & tileCache = GetTileCache();
  tileCache.lock();
  bool res = tileCache.hasTile(rectInfo);
  tileCache.unlock();
  return res;
}

void TileRenderer::AddTile(Tiler::RectInfo const & rectInfo, Tile const & tile)
{
  m_tileCache.lock();
  if (m_tileCache.hasTile(rectInfo))
  {
    m_resourceManager->renderTargetTextures()->Free(tile.m_renderTarget);
    m_tileCache.touchTile(rectInfo);
  }
  else
  {
    if (m_tileCache.canFit() == 0)
    {
      LOG(LINFO, ("resizing tileCache to", m_tileCache.cacheSize() + 1, "elements"));
      m_tileCache.resize(m_tileCache.cacheSize() + 1);
    }

    m_tileCache.addTile(rectInfo, TileCache::Entry(tile, m_resourceManager));
  }
  m_tileCache.unlock();
}

void TileRenderer::StartTile(Tiler::RectInfo const & rectInfo)
{
  threads::MutexGuard g(m_tilesInProgressMutex);
  m_tilesInProgress.insert(rectInfo);
}

void TileRenderer::FinishTile(Tiler::RectInfo const & rectInfo)
{
  threads::MutexGuard g(m_tilesInProgressMutex);
  m_tilesInProgress.erase(rectInfo);
}

void TileRenderer::SetIsPaused(bool flag)
{
  m_isPaused = flag;
}

