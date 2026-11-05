// Включаем реализацию, чтобы проверить настоящий обработчик без тестового API в приложении.
#include "../source/application.cpp"

int main() {
    using namespace application;
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    int failures = 0;
    const auto expect = [&](bool result, const char* description) {
        if (!result) { std::cerr << "FAIL: " << description << '\n'; ++failures; }
    };
    const auto near = [](Vec3 a, Vec3 b) {
        return std::sqrt(dot(subtract(a,b), subtract(a,b))) < 0.0001f;
    };
    const auto key = [&](ImGuiKey k, bool down) { io.KeysData[k - ImGuiKey_NamedKey_BEGIN].Down = down; };
    const ImGuiKey keys[] = {ImGuiKey_W, ImGuiKey_S, ImGuiKey_A, ImGuiKey_D, ImGuiKey_Q, ImGuiKey_E};
    const Vec3 directions[] = {{0,0,-1},{0,0,1},{-1,0,0},{1,0,0},{0,-1,0},{0,1,0}};
    io.WantCaptureKeyboard = io.WantCaptureMouse = false;
    for (size_t i=0; i<std::size(keys); ++i) {
        camera = Camera{};
        key(keys[i],true);
        cameraControls(0.1f);
        expect(near(camera.position,add(Vec3{0,0,6},scaleVector(directions[i],0.25f))), "W/S/A/D/Q/E direction and speed");
        key(keys[i],false);
    }
    camera = Camera{};
    key(ImGuiKey_W,true); key(ImGuiKey_D,true);
    cameraControls(0.1f);
    const Vec3 displacement=subtract(camera.position,Vec3{0,0,6});
    expect(std::abs(std::sqrt(dot(displacement,displacement))-0.25f)<0.0001f,"diagonal speed is normalized");
    key(ImGuiKey_D,false);
    io.WantCaptureKeyboard=true;
    const Vec3 before=camera.position;
    cameraControls(0.1f);
    expect(near(camera.position,before),"UI keyboard capture prevents movement");
    io.WantCaptureKeyboard=false;
    key(ImGuiKey_W,false);
    camera=Camera{}; camera.rotation={0,90,0};
    key(ImGuiKey_W,true); cameraControls(0.1f); key(ImGuiKey_W,false);
    expect(near(camera.position,{0.25f,0,6}),"movement follows rotated camera");
    camera=Camera{};
    io.MouseDown[ImGuiMouseButton_Right]=true;
    io.MouseDragMaxDistanceSqr[ImGuiMouseButton_Right]=10000;
    io.MouseDelta={20,-10};
    cameraControls(0.1f);
    expect(near(camera.rotation,{2,4,0}),"right drag yaw/pitch signs and sensitivity");
    io.WantCaptureMouse=true;
    cameraControls(0.1f);
    expect(near(camera.rotation,{2,4,0}),"UI mouse capture prevents rotation");
    io.WantCaptureMouse=false;
    io.MouseDelta={0,-10000}; cameraControls(0.1f);
    expect(camera.rotation[0]==89,"pitch pole clamp");
    io.MouseDown[ImGuiMouseButton_Right]=false;
    io.MouseDelta={100,100}; cameraControls(0.1f);
    expect(near(camera.rotation,{89,4,0}),"mouse movement without right button does not rotate");
    ImGui::DestroyContext();
    if (!failures) std::cout << "Camera input checks passed\n";
    return failures ? 1 : 0;
}
