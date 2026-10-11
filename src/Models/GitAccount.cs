using System.Text.Json.Serialization;

using CommunityToolkit.Mvvm.ComponentModel;

namespace SourceGit.Models
{
    /// <summary>
    ///     MyGitGUI: a git identity the user can switch to, globally or per repository.
    ///     Applying it writes user.name, user.email and credential.https://HOST.username,
    ///     so commits get this author and Git Credential Manager uses this account's login.
    /// </summary>
    public class GitAccount : ObservableObject
    {
        public string Name
        {
            get => _name;
            set => SetProperty(ref _name, value);
        }

        public string UserName
        {
            get => _userName;
            set => SetProperty(ref _userName, value);
        }

        public string Email
        {
            get => _email;
            set => SetProperty(ref _email, value);
        }

        public string CredentialUser
        {
            get => _credentialUser;
            set => SetProperty(ref _credentialUser, value);
        }

        public string Host
        {
            get => _host;
            set => SetProperty(ref _host, value);
        }

        [JsonIgnore]
        public string DisplayName => string.IsNullOrWhiteSpace(_name) ? (string.IsNullOrWhiteSpace(_userName) ? _email : _userName) : _name;

        [JsonIgnore]
        public string CredentialKey => $"credential.https://{(string.IsNullOrWhiteSpace(_host) ? "github.com" : _host.Trim())}.username";

        protected override void OnPropertyChanged(System.ComponentModel.PropertyChangedEventArgs e)
        {
            base.OnPropertyChanged(e);
            if (e.PropertyName is nameof(Name) or nameof(UserName) or nameof(Email))
                base.OnPropertyChanged(new System.ComponentModel.PropertyChangedEventArgs(nameof(DisplayName)));
        }

        private string _name = string.Empty;
        private string _userName = string.Empty;
        private string _email = string.Empty;
        private string _credentialUser = string.Empty;
        private string _host = "github.com";
    }
}
