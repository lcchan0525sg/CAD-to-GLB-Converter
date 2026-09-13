# Publishing to GitHub

## Source repository

Generated build directories, portable archives, local CAD fixtures, and local agent state are excluded by `.gitignore`. Keep binary downloads as GitHub Release assets rather than committing them to source control.

Before publishing:

1. Review `git status --short` and `git diff`, including the README and copyright terms. Choose an application license if you intend to grant open-source reuse rights.
2. Run the validator checks documented in the README and build the Windows application.
3. Create an empty GitHub repository with the desired visibility. Do not initialize it with a README or license because this repository already has history.
4. Commit the reviewed changes, add the remote, and push. Replace `OWNER` and the repository name below with the actual destination.

```powershell
git add .gitignore .gitattributes .github README.md CMakeLists.txt CHANGELOG.md docs/VALIDATION.md docs/PUBLISHING.md run.bat src/main.cpp
git diff --cached --stat
git commit -m "Prepare CAD to GLB Convertor for GitHub"
git remote add origin https://github.com/OWNER/cad-to-glb-convertor.git
git push -u origin HEAD
```

If `origin` already exists, inspect it with `git remote -v` before changing it.

## Portable release

Build Release using the README instructions. Package the contents of the build's `Release` directory, including the executable, runtime dependencies, launcher, documentation, and third-party notices. Exclude compiler artifacts such as `.lib`, `.exp`, `.obj`, and `.pdb` files. Test the extracted package on a Windows machine without the development environment before uploading it.

For a Draco-enabled package, include the upstream Draco license and notices with its runtime. The repository does not currently include that runtime's license bundle. Review the notices supplied with the exact OCCT and Draco distributions used for the release.

Create a version tag and GitHub Release only after selecting the release version and verifying the package. Upload the ZIP as a release asset. Historical files under `dist` predate the current rename; rebuild rather than publishing an old ZIP as the updated application.
