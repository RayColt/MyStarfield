# MyStarfield
A be nice to AI ;-), working C++ Old Skool Starfield screensaver for Windows 11.<br>

Do not forget to tweak InitStars() to your own convenience<br>
and to run and compile the project in DEBUG/x86 mode!!<br>

<img src=https://github.com/RayColt/MyStarfield/blob/master/.gitfiles/x86.jpg>

Copy generated MyStarfield.scr in Root/Debug directory to C:\Windows\System32,<br>
select it in Settings/Personalization/Lock screen/Screen saver.

<!--img src=https://github.com/RayColt/MyStarfield/blob/master/.gitfiles/MyStarfield.gif-->
<style>
    html, body {
      height: 100%;
      margin: 0;
      background: #000;
      overflow: hidden;
      font-family: "Arial", sans-serif;
    }

    /* Canvas starfield behind everything */
    #starfield {
      position: fixed;
      top: 0; left: 0;
      width: 100%;
      height: 100%;
      z-index: 0;
    }

    .title {
      font-size: 2rem;
      text-align: center;
      margin-bottom: 1rem;
    }

    .text {
      font-size: 1.25rem;
    }


    .infotxt {
      position: fixed;
      inset: 0;
      display: flex;
      align-items: center;
      justify-content: center;
      color: #7ec8ff;
      font-size: 1.25rem;
      opacity: 0;
      z-index: 1;
    }

  </style>

  <!-- Canvas starfield -->
  <canvas id="starfield"></canvas>

  <script>
    // --- Starfield animation ---
    const canvas = document.getElementById("starfield");
    const ctx = canvas.getContext("2d");
    let stars = [];

    function resize() {
      canvas.width = window.innerWidth;
      canvas.height = window.innerHeight;
      stars = [];
      for (let i = 0; i < 666; i++) {
        stars.push({
          x: Math.random() * canvas.width - canvas.width/2,
          y: Math.random() * canvas.height - canvas.height/2,
          z: Math.random() * canvas.width
        });
      }
    }
    window.addEventListener("resize", resize);
    resize();

    function animate() {
      ctx.fillStyle = "black";
      ctx.fillRect(0, 0, canvas.width, canvas.height);

      ctx.fillStyle = "white";
      for (let star of stars) {
        star.z -= 2; // speed
        if (star.z <= 0) star.z = canvas.width;

        let k = 1024.0 / star.z;
        let px = star.x * k + canvas.width / 2;
        let py = star.y * k + canvas.height / 2;

        if (px >= 0 && px < canvas.width && py >= 0 && py < canvas.height) {
          let size = (1 - star.z / canvas.width) * 2;
          ctx.fillRect(px, py, size, size);
        }
      }
      requestAnimationFrame(animate);
    }
    animate();
  </script>