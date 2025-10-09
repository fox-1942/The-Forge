#include "../../../../Common_3/Application/Interfaces/IApp.h"
#include "../../../../Common_3/Application/Interfaces/IUI.h"
#include "../../../../Common_3/Graphics/Interfaces/IGraphics.h"
#include "../../../../Common_3/Resources/ResourceLoader/Interfaces/IResourceLoader.h"
#include "../../../../Common_3/Graphics/FSL/defaults.h"
#include "../../../../Common_3/Utilities/RingBuffer.h"
#include "../../../../Common_3/Application/CameraController.cpp"
#include "./Shaders/FSL/Global.srt.h"


/*
allocate set → addDescriptorSet
bind handles into set → updateDescriptorSet
upload bytes into buffers → updateResource / map+memcpy
*/


struct UniformData
{
    CameraMatrix view;
    CameraMatrix proj;
} gUniformData;

const uint32_t gDataBufferCount = 2;

Buffer*            triangleBuffer = NULL;
Buffer*            pUniformBuffers[gDataBufferCount] = { NULL };

Pipeline*      pQuadPipeline = NULL;
DescriptorSet* pDescriptorSetTexture = { NULL };
DescriptorSet* pDescriptorSetUniforms = { NULL };

Queue*             pGraphicsQueue = NULL;
UIComponent*       pGuiWindow;
Renderer*          pRenderer = NULL;
SwapChain*         pSwapChain = NULL;
Shader*            pQuadShader = NULL;

GpuCmdRing         gGraphicsCmdRing = {};
Semaphore*         pImageAcquiredSemaphore = NULL;
uint32_t           gFrameIndex = 0;
Texture*           texture;
Sampler*           psampler;
ICameraController* pCameraController = NULL;


class MyApplication: public IApp
{
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

        TextureLoadDesc textLDesc = {};
        textLDesc.mContainer = TEXTURE_CONTAINER_DDS;
        textLDesc.pFileName = "TheForge.tex";
        textLDesc.ppTexture = &texture;
        textLDesc.mCreationFlag = TEXTURE_CREATION_FLAG_SRGB;
        addResource(&textLDesc, NULL);

        SamplerDesc samplerDesc = { FILTER_LINEAR,
                                    FILTER_LINEAR,
                                    MIPMAP_MODE_NEAREST,
                                    ADDRESS_MODE_CLAMP_TO_BORDER,
                                    ADDRESS_MODE_CLAMP_TO_BORDER,
                                    ADDRESS_MODE_CLAMP_TO_BORDER };

        addSampler(pRenderer, &samplerDesc, &psampler);

        float trianglePoints[] = {
            +0.5f, +0.5f, +1.0f, // Vertex Top Right
            0.0f,  0.0f,  -1.0f, // Normal
            1.0f,  0.0f,         // TextureCoord

            +0.5f, -0.5f, +1.0f, // Vertex Bottom Right
            0.0f,  0.0f,  -1.0f, // Normal
            1.0f,  1.0f,         // TextureCoord

            -0.5f, +0.5f, +1.0f, // Vertex Top Left
            0.0f,  0.0f,  -1.0f, // Normal
            0.0f,  0.0f,         // TextureCoord

            -0.5f, +0.5f, +1.0f, // Vertex Top Left
            0.0f,  0.0f,  -1.0f, // Normal
            0.0f,  0.0f,         // TextureCoord

            +0.5f, -0.5f, +1.0f, // Vertex Bottom Right
            0.0f,  0.0f,  -1.0f, // Normal
            1.0f,  1.0f,         // TextureCoord

            -0.5f, -0.5f, +1.0f, // Vertex Bottom Left
            0.0f,  0.0f,  -1.0f, // Normal
            0.0f,  1.0f,         // TextureCoord
        };

        size_t         triangleSize = 6 * 8 * sizeof(float);
        BufferLoadDesc triangleBufferLDesc = {};
        triangleBufferLDesc.mDesc.mDescriptors = DESCRIPTOR_TYPE_VERTEX_BUFFER;
        triangleBufferLDesc.mDesc.mMemoryUsage = RESOURCE_MEMORY_USAGE_GPU_ONLY;
        triangleBufferLDesc.mDesc.mSize = triangleSize;
        triangleBufferLDesc.mDesc.mStructStride = 8 * sizeof(float);
        triangleBufferLDesc.pData = trianglePoints;
        triangleBufferLDesc.ppBuffer = &triangleBuffer;
        addResource(&triangleBufferLDesc, NULL);

        BufferLoadDesc ubDesc = {};
        ubDesc.mDesc.mDescriptors = DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        ubDesc.mDesc.mMemoryUsage = RESOURCE_MEMORY_USAGE_CPU_TO_GPU;
        ubDesc.mDesc.mFlags = BUFFER_CREATION_FLAG_PERSISTENT_MAP_BIT;
        ubDesc.pData = NULL;


        for (uint32_t i = 0; i < gDataBufferCount; ++i)
        {
            ubDesc.mDesc.pName = "UniformBuffer";
            ubDesc.mDesc.mSize = sizeof(UniformData);
            ubDesc.ppBuffer = &pUniformBuffers[i];
            addResource(&ubDesc, NULL);
        }

        waitForAllResourceLoads();
       
        CameraMotionParameters cmp{ 40.0f, 30.0f, 200.0f };
        vec3                   camPos{ 0.0f, 0.0f, -1.0f };
        vec3                   lookAt{ 0.0f, 0.0f, 0.0f };

        pCameraController = initFpsCameraController(camPos, lookAt);

        pCameraController->setMotionParameters(cmp);

        return true;
    }

    void Exit()
    {

        removeSampler(pRenderer, psampler);

        exitCameraController(pCameraController);

        exitGpuCmdRing(pRenderer, &gGraphicsCmdRing);
        exitSemaphore(pRenderer, pImageAcquiredSemaphore);

        exitRootSignature(pRenderer);
        exitResourceLoaderInterface(pRenderer);

        exitQueue(pRenderer, pGraphicsQueue);

        exitRenderer(pRenderer);
        exitGPUConfiguration();
        pRenderer = NULL;
    }

    void addDescriptorSets()
    {
        DescriptorSetDesc descPersisent = SRT_SET_DESC(SrtData, Persistent, 1, 0);
        addDescriptorSet(pRenderer, &descPersisent, &pDescriptorSetTexture);
        DescriptorSetDesc descUniforms = SRT_SET_DESC(SrtData, PerFrame, gDataBufferCount, 0);
        addDescriptorSet(pRenderer, &descUniforms, &pDescriptorSetUniforms);
    }

    bool Load(ReloadDesc* pReloadDesc)
    {
        if (pReloadDesc->mType & RELOAD_TYPE_SHADER)
        {
            addShaders();
            addDescriptorSets();
        }

        if (pReloadDesc->mType & (RELOAD_TYPE_RESIZE | RELOAD_TYPE_RENDERTARGET))
        {
            if (!addSwapChain())
                return false;
            // (Depth is not used; you can skip addDepth() entirely if desired)

            UIComponentDesc gui = {};
            gui.mStartPosition = vec2(mSettings.mWidth * 0.01f, mSettings.mHeight * 0.2f);
            uiAddComponent(GetName(), &gui, &pGuiWindow);
        }
  
        if (pReloadDesc->mType & (RELOAD_TYPE_SHADER | RELOAD_TYPE_RENDERTARGET))
        {
            addPipelines();
        }

        PrepareDescriptorSets();

        return true;
    }

    void addPipelines()
    {
        // layout and pipeline for sphere draw
        VertexLayout vertexLayout = {};
        vertexLayout.mAttribCount = 3;
        vertexLayout.mBindingCount = 1;

        vertexLayout.mAttribs[0].mSemantic = SEMANTIC_POSITION;
        vertexLayout.mAttribs[0].mFormat = TinyImageFormat_R32G32B32_SFLOAT;
        vertexLayout.mAttribs[0].mBinding = 0;
        vertexLayout.mAttribs[0].mLocation = 0;
        vertexLayout.mAttribs[0].mOffset = 0;

        vertexLayout.mAttribs[1].mSemantic = SEMANTIC_NORMAL;
        vertexLayout.mAttribs[1].mFormat = TinyImageFormat_R32G32B32_SFLOAT;
        vertexLayout.mAttribs[1].mBinding = 0;
        vertexLayout.mAttribs[1].mLocation = 1;
        vertexLayout.mAttribs[1].mOffset = 3 * sizeof(float);

        vertexLayout.mAttribs[2].mSemantic = SEMANTIC_TEXCOORD0;
        vertexLayout.mAttribs[2].mFormat = TinyImageFormat_R32G32_SFLOAT;
        vertexLayout.mAttribs[2].mBinding = 0;
        vertexLayout.mAttribs[2].mLocation = 2;
        vertexLayout.mAttribs[2].mOffset = 6 * sizeof(float);

        PipelineDesc desc = {};
        desc.mType = PIPELINE_TYPE_GRAPHICS;
        desc.mGraphicsDesc.pVertexLayout = &vertexLayout;

        // No descriptors used → empty pipeline layout
        PIPELINE_LAYOUT_DESC(desc, SRT_LAYOUT_DESC(SrtData, Persistent), SRT_LAYOUT_DESC(SrtData, PerFrame), NULL, NULL);

        GraphicsPipelineDesc& gp = desc.mGraphicsDesc;

        gp.mPrimitiveTopo = PRIMITIVE_TOPO_TRI_LIST;
        gp.mRenderTargetCount = 1;
        gp.pDepthState = NULL;
        gp.pColorFormats = &pSwapChain->ppRenderTargets[0]->mFormat;
        gp.mSampleCount = pSwapChain->ppRenderTargets[0]->mSampleCount;
        gp.mSampleQuality = pSwapChain->ppRenderTargets[0]->mSampleQuality;
        gp.mDepthStencilFormat = TinyImageFormat_UNDEFINED;
        gp.pShaderProgram = pQuadShader;
        gp.pVertexLayout = &vertexLayout;

        RasterizerStateDesc rast = {};
        rast.mCullMode = CULL_MODE_NONE;
        gp.pRasterizerState = &rast;

        addPipeline(pRenderer, &desc, &pQuadPipeline);
    }

    void removeShaders() { removeShader(pRenderer, pQuadShader); }

    void removePipelines() { removePipeline(pRenderer, pQuadPipeline); }

    void Unload(ReloadDesc* pReloadDesc)
    {
        if (pReloadDesc->mType & RELOAD_TYPE_SHADER)
        {
            removeShaders();
            removeDescriptorSets();
        }

        waitQueueIdle(pGraphicsQueue);

        if (pReloadDesc->mType & (RELOAD_TYPE_SHADER | RELOAD_TYPE_RENDERTARGET))
        {
            removePipelines();
            removeResource(texture);
            removeResource(triangleBuffer);
          
            for (uint32_t i = 0; i < gDataBufferCount; ++i)
            {
                removeResource(pUniformBuffers[i]);
            }

        }

        if (pReloadDesc->mType & (RELOAD_TYPE_RESIZE | RELOAD_TYPE_RENDERTARGET))
        {
            removeSwapChain(pRenderer, pSwapChain);
            uiRemoveComponent(pGuiWindow);
        }
    }

    void Update(float deltaTime) override
    {
        pCameraController->update(deltaTime);

        CameraMatrix viewMat = pCameraController->getViewMatrix();

        const float  aspectInverse = (float)mSettings.mHeight / (float)mSettings.mWidth;
        const float  horizontal_fov = PI / 2.0f;
        CameraMatrix projMat = CameraMatrix::perspectiveReverseZ(horizontal_fov, aspectInverse, 0.1f, 1000.0f);
      
        gUniformData.view = viewMat;
        gUniformData.proj = projMat;


        viewMat.setTranslation(vec3(0));
        
    }

    void removeDescriptorSets()
    {
        removeDescriptorSet(pRenderer, pDescriptorSetUniforms);
        removeDescriptorSet(pRenderer, pDescriptorSetTexture);
    }

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

        // Update uniform buffers

        // Stall if CPU is running "gDataBufferCount" frames ahead of GPU
        FenceStatus fenceStatus;
        getFenceStatus(pRenderer, elem.pFence, &fenceStatus);
        if (fenceStatus == FENCE_STATUS_INCOMPLETE)
            waitForFences(pRenderer, 1, &elem.pFence);

        // Update uniform buffers
        BufferUpdateDesc viewProjCbv = { pUniformBuffers[gFrameIndex] };
        beginUpdateResource(&viewProjCbv);
        memcpy(viewProjCbv.pMappedData, &gUniformData, sizeof(gUniformData));
        endUpdateResource(&viewProjCbv);

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

        cmdBindPipeline(cmd, pQuadPipeline);
            {
                cmdBindDescriptorSet(cmd, 0, pDescriptorSetTexture);
                cmdBindDescriptorSet(cmd, gFrameIndex, pDescriptorSetUniforms);

                const uint32_t VbStride = sizeof(float) * 8;
                cmdBindVertexBuffer(cmd, 1, &triangleBuffer, &VbStride, NULL);
                cmdDraw(cmd, 6, 0);
            }
       
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
        addShader(pRenderer, &graphShader, &pQuadShader);
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

    void PrepareDescriptorSets()
    {
        DescriptorData params[2] = {};
        params[0].mIndex = SRT_RES_IDX(SrtData, Persistent, Texture);
        params[0].ppTextures = &texture;
        params[1].mIndex = SRT_RES_IDX(SrtData, Persistent, uSampler0);
        params[1].ppSamplers = &psampler;
        updateDescriptorSet(pRenderer, 0, pDescriptorSetTexture, 2, params);
       
        for (uint32_t i = 0; i < gDataBufferCount; ++i)
        {
            DescriptorData uParams[1] = {};
            uParams[0].mIndex = SRT_RES_IDX(SrtData, PerFrame, UniformData);
            uParams[0].ppBuffers = &pUniformBuffers[i];
            updateDescriptorSet(pRenderer, i, pDescriptorSetUniforms, 1, uParams);
        }
    }
};


DEFINE_APPLICATION_MAIN(MyApplication)