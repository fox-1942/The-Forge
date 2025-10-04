#include "../../../../Common_3/Application/Interfaces/IApp.h"
#include "../../../../Common_3/Application/Interfaces/IUI.h"
#include "../../../../Common_3/Graphics/Interfaces/IGraphics.h"
#include "../../../../Common_3/Resources/ResourceLoader/Interfaces/IResourceLoader.h"
#include "../../../../Common_3/Graphics/FSL/defaults.h"
#include "../../../../Common_3/Utilities/RingBuffer.h"

class MyApplication: public IApp
{
    // But we only need Two sets of resources (one in flight and one being used on CPU)
    const uint32_t gDataBufferCount = 2;

    Queue*         pGraphicsQueue = NULL;
    UIComponent*   pGuiWindow;
    Renderer*      pRenderer = NULL;
    SwapChain*     pSwapChain = NULL;
    Shader*        pGraphShader = NULL;
    Pipeline*      pSpherePipeline = NULL;
    GpuCmdRing     gGraphicsCmdRing = {};
    Semaphore*     pImageAcquiredSemaphore = NULL;
    uint32_t       gFrameIndex = 0;

    bool Init() override
    {
        // window and renderer setup
        RendererDesc settings;
        memset(&settings, 0, sizeof(settings));
        initGPUConfiguration(settings.pExtendedSettings);
        initRenderer(GetName(), &settings, &pRenderer);
        // check for init success
        if (!pRenderer)
        {
            ShowUnsupportedMessage(getUnsupportedGPUMsg());
            return false;
        }
        setupGPUConfigurationPlatformParameters(pRenderer, settings.pExtendedSettings);

        QueueDesc queueDesc = {};
        queueDesc.mType = QUEUE_TYPE_GRAPHICS;
        queueDesc.mFlag = QUEUE_FLAG_INIT_MICROPROFILE;
        initQueue(pRenderer, &queueDesc, &pGraphicsQueue);

        GpuCmdRingDesc cmdRingDesc = {};
        cmdRingDesc.pQueue = pGraphicsQueue;
        cmdRingDesc.mPoolCount = gDataBufferCount;
        cmdRingDesc.mCmdPerPoolCount = 1;
        cmdRingDesc.mAddSyncPrimitives = true;
        initGpuCmdRing(pRenderer, &cmdRingDesc, &gGraphicsCmdRing);

        initSemaphore(pRenderer, &pImageAcquiredSemaphore);

        initResourceLoaderInterface(pRenderer);

        RootSignatureDesc rootDesc = {};
        INIT_RS_DESC(rootDesc, "default.rootsig", "compute.rootsig");
        initRootSignature(pRenderer, &rootDesc);

       
        return true;
    }

    void Exit()
    {
        exitGpuCmdRing(pRenderer, &gGraphicsCmdRing);
        exitSemaphore(pRenderer, pImageAcquiredSemaphore);

        exitRootSignature(pRenderer);
        exitResourceLoaderInterface(pRenderer);

        exitQueue(pRenderer, pGraphicsQueue);

        exitRenderer(pRenderer);
        exitGPUConfiguration();
        pRenderer = NULL;
    }

    bool Load(ReloadDesc* pReloadDesc)
    {
        if (pReloadDesc->mType & (RELOAD_TYPE_RESIZE | RELOAD_TYPE_RENDERTARGET))
        {
            if (!addSwapChain())
                return false;
            // (Depth is not used; you can skip addDepth() entirely if desired)

            UIComponentDesc gui = {};
            gui.mStartPosition = vec2(mSettings.mWidth * 0.01f, mSettings.mHeight * 0.2f);
            uiAddComponent(GetName(), &gui, &pGuiWindow);
        }

        if (pReloadDesc->mType & RELOAD_TYPE_SHADER)
        {
            addShaders();
        }

        if (pReloadDesc->mType & (RELOAD_TYPE_SHADER | RELOAD_TYPE_RENDERTARGET))
        {
            addPipelines();
        }

        return true;
    }

    void addPipelines()
    {
        PipelineDesc desc = {};
        desc.mType = PIPELINE_TYPE_GRAPHICS;

        // No descriptors used → empty pipeline layout
        PIPELINE_LAYOUT_DESC(desc, NULL, NULL, NULL, NULL);

        GraphicsPipelineDesc& gp = desc.mGraphicsDesc;
        gp.mPrimitiveTopo = PRIMITIVE_TOPO_TRI_LIST;
        gp.mRenderTargetCount = 1;
        gp.pColorFormats = &pSwapChain->ppRenderTargets[0]->mFormat;
        gp.mSampleCount = pSwapChain->ppRenderTargets[0]->mSampleCount;
        gp.mSampleQuality = pSwapChain->ppRenderTargets[0]->mSampleQuality;

        gp.mDepthStencilFormat = TinyImageFormat_UNDEFINED;
        gp.pDepthState = NULL;
        gp.pVertexLayout = NULL;

        static RasterizerStateDesc rast = {};
        rast.mCullMode = CULL_MODE_NONE;
        gp.pRasterizerState = &rast;

        gp.pShaderProgram = pGraphShader;

        addPipeline(pRenderer, &desc, &pSpherePipeline);
    }

    void removeShaders() { removeShader(pRenderer, pGraphShader); }

    void removePipelines() { removePipeline(pRenderer, pSpherePipeline); }

    void Unload(ReloadDesc* pReloadDesc)
    {
        if (pReloadDesc->mType & RELOAD_TYPE_SHADER)
        {
            removeShaders();
        }

        waitQueueIdle(pGraphicsQueue);

        if (pReloadDesc->mType & (RELOAD_TYPE_SHADER | RELOAD_TYPE_RENDERTARGET))
        {
            removePipelines();
        }

        if (pReloadDesc->mType & (RELOAD_TYPE_RESIZE | RELOAD_TYPE_RENDERTARGET))
        {
            removeSwapChain(pRenderer, pSwapChain);
            uiRemoveComponent(pGuiWindow);
        }
    };

    void Update(float deltaTime) override { deltaTime; }

    void Draw() override
    {
        if ((bool)pSwapChain->mEnableVsync != mSettings.mVSyncEnabled)
        {
            waitQueueIdle(pGraphicsQueue);
            ::toggleVSync(pRenderer, &pSwapChain);
        }

        uint32_t swapchainImageIndex;
        acquireNextImage(pRenderer, pSwapChain, pImageAcquiredSemaphore, NULL, &swapchainImageIndex);

        pSwapChain->mImageCount;

        RenderTarget*     pRenderTarget = pSwapChain->ppRenderTargets[swapchainImageIndex];
        GpuCmdRingElement elem = getNextGpuCmdRingElement(&gGraphicsCmdRing, true, 1);

        // Stall if CPU is running "gDataBufferCount" frames ahead of GPU
        FenceStatus fenceStatus;
        getFenceStatus(pRenderer, elem.pFence, &fenceStatus);
        if (fenceStatus == FENCE_STATUS_INCOMPLETE)
            waitForFences(pRenderer, 1, &elem.pFence);

        resetCmdPool(pRenderer, elem.pCmdPool);

        Cmd* cmd = elem.pCmds[0];
        beginCmd(cmd);

        // Transition to RT
        {
            RenderTargetBarrier barrier = { pRenderTarget, RESOURCE_STATE_PRESENT, RESOURCE_STATE_RENDER_TARGET };
            cmdResourceBarrier(cmd, 0, NULL, 0, NULL, 1, &barrier);
        }

        // Bind and clear RT
        {
            BindRenderTargetsDesc bind = {};
            bind.mRenderTargetCount = 1;
            bind.mRenderTargets[0] = { pRenderTarget, LOAD_ACTION_CLEAR };
            bind.mDepthStencil = { NULL, LOAD_ACTION_DONTCARE };
            cmdBindRenderTargets(cmd, &bind);

            cmdSetViewport(cmd, 0.0f, 0.0f, (float)pRenderTarget->mWidth, (float)pRenderTarget->mHeight, 0.0f, 1.0f);
            cmdSetScissor(cmd, 0, 0, pRenderTarget->mWidth, pRenderTarget->mHeight);
        }

        // Draw fullscreen triangle (VS uses SV_VertexID)
        cmdBindPipeline(cmd, pSpherePipeline);
        cmdDraw(cmd, 4, 0);

        // Transition back to present
        cmdBindRenderTargets(cmd, NULL);
        {
            RenderTargetBarrier barrier = { pRenderTarget, RESOURCE_STATE_RENDER_TARGET, RESOURCE_STATE_PRESENT };
            cmdResourceBarrier(cmd, 0, NULL, 0, NULL, 1, &barrier);
        }

        endCmd(cmd);

        // Submit & present
        FlushResourceUpdateDesc flush = {};
        flush.mNodeIndex = 0;
        flushResourceUpdates(&flush);
        Semaphore* waits[2] = { flush.pOutSubmittedSemaphore, pImageAcquiredSemaphore };

        QueueSubmitDesc submit = {};
        submit.mCmdCount = 1;
        submit.ppCmds = &cmd;
        submit.mWaitSemaphoreCount = TF_ARRAY_COUNT(waits);
        submit.ppWaitSemaphores = waits;
        submit.mSignalSemaphoreCount = 1;
        submit.ppSignalSemaphores = &elem.pSemaphore;
        submit.pSignalFence = elem.pFence;
        queueSubmit(pGraphicsQueue, &submit);

        QueuePresentDesc present = {};
        present.pSwapChain = pSwapChain;
        present.mIndex = (uint8_t)swapchainImageIndex;
        present.mWaitSemaphoreCount = 1;
        present.ppWaitSemaphores = &elem.pSemaphore;
        present.mSubmitDone = true;
        queuePresent(pGraphicsQueue, &present);

        gFrameIndex = (gFrameIndex + 1u) % gDataBufferCount;
    }

    const char* GetName() override { return "name"; }

    void addShaders()
    {
        ShaderLoadDesc graphShader = {};
        graphShader.mVert.pFileName = "basic.vert";
        graphShader.mFrag.pFileName = "basic.frag";
        addShader(pRenderer, &graphShader, &pGraphShader);
    }

    bool addSwapChain()
    {
        SwapChainDesc sc = {};
        sc.mWindowHandle = pWindow->handle;
        sc.mPresentQueueCount = 1;
        sc.ppPresentQueues = &pGraphicsQueue;
        sc.mWidth = mSettings.mWidth;
        sc.mHeight = mSettings.mHeight;
        sc.mImageCount = getRecommendedSwapchainImageCount(pRenderer, &pWindow->handle);
        sc.mColorSpace = COLOR_SPACE_SDR_SRGB;
        sc.mColorFormat = getSupportedSwapchainFormat(pRenderer, &sc, sc.mColorSpace);
        sc.mEnableVsync = mSettings.mVSyncEnabled;
        ::addSwapChain(pRenderer, &sc, &pSwapChain);
        return pSwapChain != NULL;
    }
};

DEFINE_APPLICATION_MAIN(MyApplication)