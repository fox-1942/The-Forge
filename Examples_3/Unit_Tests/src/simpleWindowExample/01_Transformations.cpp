#include "../../../../Common_3/Application/Interfaces/IApp.h"

class MyApplication: public IApp
{
public:
    bool Init() override { return true; }
    void Exit() override {}

    bool Load(ReloadDesc* pReloadDesc)
    {
        pReloadDesc;
        return true;
    };
    void Unload(ReloadDesc* pReloadDesc) { pReloadDesc; };

    void Update(float deltaTime) override { deltaTime; }
    void Draw() override {}

    const char* GetName() override { return "name"; }
};

extern int WindowsMain(int argc, char** argv, IApp* app);

int main(int argc, char** argv)
{
    MyApplication app;
    return WindowsMain(argc, argv, &app);
}