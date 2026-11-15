using System;
using System.Windows;
using System.Windows.Navigation;
using Microsoft.Phone.Controls;

namespace RuffleWP80
{
    public partial class App : Application
    {
        public static PhoneApplicationFrame RootFrame { get; private set; }

        public App()
        {
            InitializeComponent();
            RootFrame = new PhoneApplicationFrame();
            RootFrame.UriMapper = new AssociationUriMapper();
            RootFrame.Navigated += (s, e) =>
            {
                if (RootVisual != RootFrame) RootVisual = RootFrame;
            };
        }
    }

    // Mở app bằng file .swf: hệ thống điều hướng tới /FileTypeAssociation?fileToken=... -> chuyển về MainPage
    internal class AssociationUriMapper : UriMapperBase
    {
        public override Uri MapUri(Uri uri)
        {
            string s = System.Net.HttpUtility.UrlDecode(uri.ToString());
            if (s.Contains("/FileTypeAssociation"))
            {
                int i = s.IndexOf("fileToken=", StringComparison.Ordinal);
                if (i >= 0)
                    return new Uri("/MainPage.xaml?fileToken=" + s.Substring(i + "fileToken=".Length), UriKind.Relative);
            }
            return uri;
        }
    }
}
