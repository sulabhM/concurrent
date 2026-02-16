# Push this project to a new GitHub repo

Do this **after** installing Git (e.g. `sudo apt install git` on Debian/Ubuntu).

## 1. One-time: configure Git (if not already)

```bash
git config --global user.name "Your Name"
git config --global user.email "your.email@example.com"
```

## 2. Create the repo and first commit (in this directory)

```bash
cd /home/sulabh/projects

git init
git add .
git commit -m "Initial commit: lock-free concurrent list with BSD-style macros"
```

## 3. Create the repo on GitHub and push

**Option A – GitHub CLI (`gh`)**

If you have [GitHub CLI](https://cli.github.com/) installed and logged in:

```bash
gh repo create concurrent-list --source=. --remote=origin --push --private
# use --public instead of --private if you want a public repo
```

**Option B – Manual (GitHub website)**

1. On [github.com](https://github.com), click **New repository**.
2. Name it (e.g. `concurrent-list`), choose public/private, **do not** add a README or .gitignore.
3. In this directory:

   ```bash
   git remote add origin https://github.com/YOUR_USERNAME/concurrent-list.git
   git branch -M main
   git push -u origin main
   ```

   Replace `YOUR_USERNAME` and `concurrent-list` with your GitHub username and repo name. Use the SSH URL (`git@github.com:...`) if you use SSH keys.

## 4. Optional: ignore the build directory

The existing `.gitignore` already ignores `build/`, so the `build/` folder will not be committed.
