# SimCook: alat berkas. Tidak menautkan perender maupun editor — alat berkas
# yang menuntut GPU adalah alat yang tidak bisa dijalankan di CI, dan CI adalah
# satu-satunya tempat ini akan dijalankan berulang kali.
sim_add_executable(SimCook
    SOURCES src/main.cpp
    DEPS
        Sim::Core
        Sim::Assets
        Sim::Scene)
